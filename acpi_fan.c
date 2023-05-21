/* FreeBSD fan driver */
/* current ACPI specification 6.5 */

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
	int			version;	/* either ACPI 1.0 or 4.0 */
	
	bool		fan_runs;
	int 		fan_level;
	int			fan_speed;
	
	struct 		acpi_fif;
	struct 		acpi_fps;
	struct 		acpi_fst;
};

/* (dynamic) sysctls for fan speed and level (acpi4 only) */
static struct	sysctl_ctx_list clist;


static device_method_t acpi_fan_methods[] = {
    
	/* Device interface */
    DEVMETHOD(device_probe,		acpi_fan_probe),
    DEVMETHOD(device_attach,	acpi_fan_attach),
    DEVMETHOD(device_detach,	acpi_fan_detach),
	/* XXX: Do we need device_suspend? */

    DEVMETHOD_END
};

/* **************** */
/* helper functions */
/* **************** */
static int acpi_fan_get_fif(device_t dev);
static void acpi_fan_initiate_acpi4(device_t);
static void acpi_fan_turn_on_off(bool powered);
static void acpi_fan_set_level(int new_fan_level);
static void acpi_fan_set_level(int);

static void acpi_fan_turn_on_off(SYSCTL_HANDLER_ARGS);
static int acpi_fan_level_sysctl(SYSCTL_HANDLER_ARGS);
static int acpi_fan_speed_sysctl(SYSCTL_HANDLER_ARGS);


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

	sysctl_ctx_init(&clist);		/* sysctl context */

	/* fans are either acpi 1.0 or 4.0 compatible, so check now. */
	if (acpi_fan_get_fif(dev) &&
	ACPI_SUCCESS(acpi_get_handle(handle, "_FPS", &tmp)) &&
	ACPI_SUCCESS(acpi_get_handle(handle, "_FSL", &tmp)) &&
	ACPI_SUCCESS(acpi_get_handle(handle, "_FST", &tmp)))	
		acpi_fan_initiate_acpi4(dev);
	
	else	/* nothing to do in acpi version 1, really */
		acpi_fan_softc.version = 1;

	/* Since the operating system has access to this fan, we can turn it on/off 
	in all scenarios. */
	struct sysctl_oid *fan_oid = device_get_sysctl_tree(dev);
	SYSCTL_ADD_PROC(&clist, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
	"Fan running", CTLTYPE_INT | CTLFLAG_RW, 0, 0,
	acpi_fan_turn_on_off, "I" ,"Fan running");

	return 0;
}

static int
acpi_fan_detach(device_t dev) {
	sysctl_ctx_free(&clist);
	return 0;
}

/* XXX: initialization needs to be programmed still */
static void acpi_fan_initiate_acpi4(device_t dev) {
	acpi_fan_softc.version = 4;

	struct sysctl_oid *fan_oid = device_get_sysctl_tree(dev);
	SYSCTL_ADD_PROC(&clist, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
	"Fan level", CTLTYPE_INT | CTLFLAG_RW, 0, 0,
	acpi_fan_level_sysctl, "I" ,"Fan level");

	struct sysctl_oid *fan_oid = device_get_sysctl_tree(dev);
	SYSCTL_ADD_PROC(&clist, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
	"Fan speed", CTLTYPE_INT | CTLFLAG_RW, 0, 0,
	acpi_fan_speed_sysctl, "I" ,"Fan speed");
}

static void
acpi_fan_turn_on_off(SYSCTL_HANDLER_ARGS) {

	struct sysctl_oid *parent;
    struct acpi_fan_softc *sc;
    device_t dev;
    ACPI_HANDLE h;
	ACPI_STATUS status;
	long fan_index;
	int fan_runs;
	

    parent = SYSCTL_PARENT(oidp); 
    fan_index = strtol(parent->oid_name, NULL, 0);
    dev = devclass_get_device(acpi_fan_devclass, (int) fan_index);

    h = acpi_get_handle(dev);		
    sc = device_get_softc(dev);
	

    if(req->newptr) {	/* Write request */
		
		SYSCTL_IN(req, &fan_runs, sizeof(fan_runs));
		
			/* Set power states D3 or D0 */
			if(fan_runs) {
				/* set power state D3 */
				//XXX: acpi_set_powerstate(dev, ACPI_STATE_D3) ??;
				status = acpi_evaluate_object(h, "_PS3", NULL, NULL);
-				if (ACPI_FAILURE(status)) {
-					ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
					"turning fan on: failed\n");
					return 0;
				}
				sc->fan_runs = TRUE;
			}
		
			else {
				/* set power state D0 */
				//XXX: acpi_set_powerstate(dev, ACPI_STATE_D0);
				status = acpi_evaluate_object(h, "_PS0", NULL, NULL);
-				if (ACPI_FAILURE(status)) {
-					ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
					"turning fan off: failed\n");
					return 0;
				}
				sc->fan_runs = FALSE;
			}
	}

    else /* read request */ {
			/* get the power state, report it. */
			&sc->fan_runs = acpi_get_powerstate(dev); /* XXX: what is this? */
			SYSCTL_OUT(req, &sc->fan_runs, sizeof(sc->fan_on_off));
	}
    return 0;
}

static void
acpi_fan_set_level(int new_request) {
	printf("setting new fan level?!");
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
	int fan_new_level;

    parent = SYSCTL_PARENT(oidp); 
    fan_index = strtol(parent->oid_name, NULL, 0);
    dev = devclass_get_device(acpi_fan_devclass, (int) fan_index);

    h = acpi_get_handle(dev);		
    sc = device_get_softc(dev);

    if(req->newptr) {	/* Write request */
		
		SYSCTL_IN(req, &fan_new_level, sizeof(fan_new_level));
		acpi_fan_set_level(new_level);
	}

    else /* read request */ {
			SYSCTL_OUT(req, &sc->fan_level, sizeof(sc->fan_level)); // instead probe _FST???
	}
    return 0;
}

static int
acpi_fan_speed_sysctl(SYSCTL_HANDLER_ARGS) {
return 0;
}

static int acpi_fan_get_fif(device_t dev) {
	
    struct acpi_cmbat_softc *sc;
    ACPI_STATUS	as;
	ACPI_HANDLE	h;
	ACPI_HANDLE tmp;
	ACPI_BUFFER	fif_buffer;
	
	sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
	
	
	if(ACPI_FAILURE(acpi_get_handle(handle, "_FIF", &tmp))
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


/* ******************* */
/* Register the driver */
/* ******************* */
static driver_t acpi_fan_driver = {
    "fan",
    acpi_fan_methods,
    sizeof(struct acpi_fan_softc),
};

DRIVER_MODULE(acpi_fan, acpi, acpi_fan_driver, 0, 0);
MODULE_DEPEND(acpi_fan, acpi, 1, 1, 1);