/* ******************************* */
/* FreeBSD acpi generic fan driver */
/* current ACPI specification: 6.5 */
/* ******************************* */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_acpi.h"
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>

/* for testing, aka printf */
#include <sys/types.h>
#include <sys/systm.h>

#include <contrib/dev/acpica/include/acpi.h>

#include <dev/acpica/acpivar.h>
#include <dev/acpica/acpiio.h>

/* Hooks for the ACPI CA debugging infrastructure */
#define	_COMPONENT	ACPI_FAN
ACPI_MODULE_NAME("FAN")


/* ****************************************************************** */
/* structures for _FPS, _FIF, _FST (aka acpi version 4.0 fan control) */
/* ****************************************************************** */

struct acpi_fan_fps {
	int control;
	int trip_point;
	int speed;
	int noise_level;
	int power;
	char name[ACPI_FPS_NAME_LEN];
	struct device_attribute dev_attr;
};

struct acpi_fif {
	int rev;	/* revision always zero */
	int fine_grain_ctrl;	/* fine grain control, 0-100 */
	int stepsize;	/* step size 1-9 */
	int low_fanspeed;	/* low fan speed notification, 0x80, either zero or nonzero */
};

struct acpi_fan_fst {
	int revision;
	int control;
	int speed;
};

/* *********************** */
/* driver software context */
/* *********************** */

struct acpi_fan_softc {
	device_t	dev;
	int			acpi4;	/* either ACPI 1.0 or 4.0 */
	
	int			fan_is_running;
	int			fan_speed;
	int 		fan_level;
	
	struct 		acpi_fif;
	struct 		acpi_fps;
	struct 		acpi_fst;
};

/* (dynamic) sysctls */
static struct	sysctl_ctx_list clist;


static device_method_t acpi_fan_methods[] = {
    
	/* Device interface */
    DEVMETHOD(device_probe,		acpi_fan_probe),
    DEVMETHOD(device_attach,	acpi_fan_attach),
    DEVMETHOD(device_detach,	acpi_fan_detach),
	DEVMETHOD(device_suspend,	acpi_fan_suspend),
	DEVMETHOD(device_resume,	acpi_fan_resume),
	
    DEVMETHOD_END
};

/* ---------------- *
 * helper functions *
 * ---------------- */
static int acpi_fan_get_fif(device_t dev);
static int acpi_fan_get_fst(device_t dev);
static int acpi_fan_level_sysctl(SYSCTL_HANDLER_ARGS)
static int acpi_fan_on_sysctl(SYSCTL_HANDLER_ARGS);
static void acpi_fan_set_on(device_t dev, int new_state);

/* probe the fan */
static int
acpi_fan_probe(device_t dev)
{
    static char *fan_ids[] = { \
	"PNP0C0B", 		/* Generic Fan */ \
	"INT3404",		/* Fan */ \
	"INTC1044",		/* Fan for Tiger Lake generation */ \
	"INTC1048", 	/* Fan for Alder Lake generation */ \
	"INTC1063", 	/* Fan for Meteor Lake generation */ \
	"INTC10A2", 	/* Fan for Raptor Lake generation */ \
	NULL };
    int rv;
    
    if (acpi_disabled("fan"))
	return (ENXIO);
    rv = ACPI_ID_PROBE(device_get_parent(dev), dev, fan_ids, NULL);
    if (rv <= 0)
	device_set_desc(dev, "ACPI FAN");
	/* XXX: we can do strncmp and then set a more precise description. */
	/* For now, this will do. */

    return (rv);
}


static int
acpi_fan_attach(device_t dev)
{
	int	error;
	ACPI_HANDLE	handle;
	ACPI_HANDLE tmp;
	struct acpi_fan_softc *sc;

	
    sc = device_get_softc(dev);
    handle = acpi_get_handle(dev);
    sc->dev = dev;

	/* create sysctls for 3 scenarios: 
	fan control via percentage (1)
	fan control via fan levels (2)
	fan control via acpi version 1.0 (3) */
	
	sysctl_ctx_init(&clist);		/* sysctl context */
	struct sysctl_oid *fan_oid = device_get_sysctl_tree(dev);


	/* fans are either acpi 1.0 or 4.0 compatible, so check now. */
	if (acpi_fan_get_fif(dev) &&
		acpi_fan_get_fst(dev) &&
		ACPI_SUCCESS(acpi_GetHandleInScope(handle, "_FPS", &tmp)) &&	/* XXX: needs correction */
		ACPI_SUCCESS(acpi_GetHandleInScope(handle, "_FSL", &tmp)))
		{
		acpi_fan_softc.acpi4=1;
		
		if(sc.fps->fine_grain_ctrl) {
			SYSCTL_ADD_PROC(&clist, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
			"fan_speed", CTLTYPE_INT | CTLFLAG_RW, 0, 0,
			acpi_fan_level_sysctl, "I" ,"Fan speed in %");

			SYSCTL_ADD_INT(&clist, SYSCTL_CHILDREN(fan_oid), OID_AUTO, acpi_fan_softc.acpi_fif->stepsize,
			"Step_size", CTLTYPE_INT | CTLFLAG_R, 0, 0, "I" ,"Step size");
		}
	else {
		SYSCTL_ADD_PROC(&clist, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
		"current_fan_level", CTLTYPE_INT | CTLFLAG_RW, 0, 0,
		acpi_fan_level_sysctl, "I" ,"Fan level");
		
		/* XXX: available fan levels, string? array?*/
		SYSCTL_ADD_INT(&clist, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
		"fan levels", CTLTYPE_INT | CTLFLAG_R, 0, 0, "I" ,"available fan levels");
	}
	
	/* fan status XXX: string? */
	SYSCTL_ADD_INT(&clist, SYSCTL_CHILDREN(fan_oid), OID_AUTO, acpi_fan_softc.acpi_fif->stepsize,
			"Fan_status", CTLTYPE_INT | CTLFLAG_R, 0, 0, "I" ,"Fan statuts");
	}

	else {	/* acpi0 */
		acpi_fan_softc.acpi4 = 0;
		SYSCTL_ADD_PROC(&clist, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
		"Fan_on", CTLTYPE_INT | CTLFLAG_RW, 0, 0,
		acpi_fan_on_sysctl, "I" ,"Fan ON=1 OFF=0");
		/* acpi subsystem powers on all new devices, right? No need to check */
		sc.fan_is_running=1;
	}

	return 0;
}

static int
acpi_fan_detach(device_t dev) {
	sysctl_ctx_free(&clist);
	return 0;
}

static int
acpi_fan_suspend(device_t dev) {
	acpi_fan_set_on(dev, 0);
	return 0;
}

static int
acpi_fan_resume(device_t dev) {
	acpi_fan_set_on(dev, 1);
	return 0;
}


static int
acpi_fan_level_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sysctl_oid *parent;
    struct acpi_fan_softc *sc;
    device_t dev;
    ACPI_HANDLE h;
	ACPI_STATUS status;
	long fan_index;
	int requested_speed;

    parent = SYSCTL_PARENT(oidp); 
    fan_index = strtol(parent->oid_name, NULL, 0);
    dev = devclass_get_device(acpi_fan_devclass, (int) fan_index);

    h = acpi_get_handle(dev);		
    sc = device_get_softc(dev);

    if(req->newptr) {	/* Write request */
		
		if(!sc->fan_is_running))
			acpi_fan_set_on(dev, 1);
			
		if(sc->fif.fine_grain_ctrl) { /* fan is set via percentage: 0-100 % */
			
			/* check input */
			SYSCTL_IN(req, &requested_speed, sizeof(requested_speed));
			if((requested_speed <= 100) && (requested_speed >= 0))
				/* XXX: todo: call FST */
				nop;
			/* else: invalid input */
		}
		
		else {	/* fan is set via levels */ 
			/* XXX: todo: call FST */
			nop;
		}
	}

    else /* read request */ {
			SYSCTL_OUT(req, &sc->fan_level, sizeof(sc->fan_level)); // instead probe _FST???
	}
    return 0;
}

static int
acpi_fan_on_sysctl(SYSCTL_HANDLER_ARGS) {
	

	struct sysctl_oid *parent;
    struct acpi_fan_softc *sc;
    device_t dev;
    ACPI_HANDLE h;
	ACPI_STATUS status;
	long fan_index;
	int fan_new;
	

    parent = SYSCTL_PARENT(oidp); 
    fan_index = strtol(parent->oid_name, NULL, 0);
    dev = devclass_get_device(acpi_fan_devclass, (int) fan_index);

    h = acpi_get_handle(dev);		
    sc = device_get_softc(dev);
	

    if(req->newptr) {	/* Write request */
		
		SYSCTL_IN(req, &fan_new, sizeof(fan_runs));
		if((fan_new == 1) || (fan_new == 0))
			acpi_fan_set_on(dev, fan_new);

		/* else error */
	}

    else /* read request */ {
			/* get the power state, report it. */
			&sc->fan_is_running = acpi_get_powerstate(dev); /* XXX: what is this? */
			SYSCTL_OUT(req, &sc->fan_is_running, sizeof(sc->fan_is_running));
	}
    return 0;
}

static void
acpi_fan_set_on(device_t dev, int new_state) {

	struct acpi_fan_softc *sc;
    ACPI_HANDLE h;
	ACPI_STATUS status;

    h = acpi_get_handle(dev);		
    sc = device_get_softc(dev);

		if(new_state) {
			/* set fan to  D3 (On) */
			//XXX: status = acpi_set_powerstate(dev, ACPI_STATE_D3) ??;
			status = acpi_evaluate_object(h, "_PS3", NULL, NULL);
-			if (ACPI_FAILURE(status)) {
-				ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
				"turning fan on: failed --%s\n", AcpiFormatException(statuts));
				return;
			}
			sc->fan_is_running = 1;
		}
	
		else {
		/* set fan to  D0 (Off) */
			//XXX: status = acpi_set_powerstate(dev, ACPI_STATE_D0) ??;
			status = acpi_pwr_switch_consumer(h, ACPI_STATE_D0);
			// status = acpi_evaluate_object(h, "_PS0", NULL, NULL);
-			if (ACPI_FAILURE(status)) {
-				ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
				"turning fan off: failed --%s\n", AcpiFormatException(statuts));
				return;
			}
			sc->fan_is_running = 0;
		}
}

static int acpi_fan_get_fif(device_t dev) {
	
    struct acpi_cmbat_softc *sc;
    ACPI_STATUS	as;
	ACPI_HANDLE	h;
	ACPI_HANDLE tmp;
	ACPI_BUFFER	fif_buffer;
	
	sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
	
	
	if(ACPI_FAILURE(acpi_get_handle(handle, "_FIF", &tmp)))
		return 0;
	
	
	as = AcpiEvaluateObject(h, "_FIF", NULL, &fif_buffer);
    if (ACPI_FAILURE(as)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
	    "error fetching current fan status -- %s\n",
	    AcpiFormatException(as));
		return 0;
	}
	memcpy(fif_buffer, &sc->acpi_fif, sizeof(*fif_buffer));
	return 1;
}


static int acpi_fan_get_fst(device_t dev) {
		
    struct acpi_cmbat_softc *sc;
    ACPI_STATUS	as;
	ACPI_HANDLE	h;
	ACPI_HANDLE tmp;
	ACPI_BUFFER	fst_buffer;
	
	sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
	
	
	if(ACPI_FAILURE(acpi_get_handle(handle, "_FST", &tmp)))
		return 0;
	
	as = AcpiEvaluateObject(h, "_FST", NULL, &fst_buffer);
    if (ACPI_FAILURE(as)) {
		ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
	    "error fetching current fan status -- %s\n",
	    AcpiFormatException(as));
		return 0;
	}
	memcpy(fst_buffer, &sc->acpi_fst, sizeof(*fst_buffer));
	return 1;
}


/* ------------------- */
/* Register the driver */
/* ------------------- */
static driver_t acpi_fan_driver = {
    "fan",
    acpi_fan_methods,
    sizeof(struct acpi_fan_softc),
};

DRIVER_MODULE(acpi_fan, acpi, acpi_fan_driver, 0, 0);
MODULE_DEPEND(acpi_fan, acpi, 1, 1, 1);