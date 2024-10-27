/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2023 Georg Lindenberg
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* -------------------------------	*/
/* FreeBSD acpi generic fan driver	*/
/* ACPI specification: 6.5			*/
/* Section: 11.3 					*/
/* -------------------------------	*/

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_acpi.h"
#include <sys/param.h>
// //#include <sys/kobj.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>


#include <sys/types.h>
#include <sys/malloc.h>

#include <sys/sysctl.h>

#include <machine/bus.h>
#include <sys/rman.h>

#include <contrib/dev/acpica/include/acpi.h>

#include <dev/acpica/acpivar.h>
#include <dev/acpica/acpiio.h>

/* Hooks for the ACPI CA debugging infrastructure */
#define	_COMPONENT	ACPI_FAN
ACPI_MODULE_NAME("FAN")

static MALLOC_DEFINE(M_ACPIFAN, "acpifan",
    "ACPI fan performance states data");


ACPI_SERIAL_DECL(fan, "ACPI fan");

/* ********************************************************************* */
/* structures required by acpi version 4.0 fan control: _FPS, _FIF, _FST */
/* ********************************************************************* */


struct acpi_fan_fps {
	int control;
	int trip_point;
	int speed;
	int noise_level;
	int power;
};

struct acpi_fan_fif {
	int rev;	/* revision always zero */
	int fine_grain_ctrl;	/* fine grain control */
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
	
	int			fan_powered;

	struct acpi_fan_fif		fif;
	struct acpi_fan_fps		fps;
	//ACPI_OBJECT *acpi_fan_fps;
	int					max_fps;
	struct acpi_fan_fst		fst;
};

static devclass_t acpi_fan_devclass;

/* (dynamic) sysctls */


/* ---------------- *
 * helper functions *
 * ---------------- */
static int acpi_fan_get_fif(device_t dev);
static int acpi_fan_get_fst(device_t dev);
static int acpi_fan_get_fps(device_t dev);
static int acpi_fan_level_sysctl(SYSCTL_HANDLER_ARGS);
static int acpi_fan_powered_sysctl(SYSCTL_HANDLER_ARGS);
static int acpi_fan_rpm_sysctl(SYSCTL_HANDLER_ARGS);
static void acpi_fan_set_power(device_t dev, int new_state);
static int acpi_fan_get_power_state(device_t dev);


/*-------------- * 
 *Device methods *
 * ------------- */
static int acpi_fan_probe(device_t dev);
static int acpi_fan_attach(device_t dev);
static int acpi_fan_detach(device_t dev);
static int acpi_fan_suspend(device_t dev);
static int acpi_fan_resume(device_t dev);

static device_method_t acpi_fan_methods[] = {
    
	/* Device interface */
    DEVMETHOD(device_probe,		acpi_fan_probe),
    DEVMETHOD(device_attach,	acpi_fan_attach),
    DEVMETHOD(device_detach,	acpi_fan_detach),
	DEVMETHOD(device_suspend,	acpi_fan_suspend),
	DEVMETHOD(device_resume,	acpi_fan_resume),
	
    DEVMETHOD_END
};


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
//	int	error;
	ACPI_HANDLE	handle;
	ACPI_HANDLE tmp;
	struct acpi_fan_softc *sc;

	
    sc = device_get_softc(dev);
    handle = acpi_get_handle(dev);
    sc->dev = dev;

	/* acpi subsystem powers on all new devices, right? No need to check. XXX: btw this is not a check. */
	sc->fan_powered=1;

	/* create sysctls for 3 scenarios: 
	fan control via percentage (1)
	fan control via fan levels (2)
	fan control via acpi version 1.0 (3) */

	/* sysctl context XXX: Delete? */
	struct sysctl_oid *fan_oid = device_get_sysctl_tree(dev);


	/* fans are either acpi 1.0 or 4.0 compatible, so check now. */
	if (acpi_fan_get_fif(dev) &&
		acpi_fan_get_fst(dev) &&
		acpi_fan_get_fps(dev) &&
		ACPI_SUCCESS(acpi_GetHandleInScope(handle, "_FSL", &tmp))) {
		
		sc->acpi4=1;	/* acpi 4.0 compatible */
		/* XXX: ACPI 4.0 will be implemented later!!!! */
		}
		
		/*
		// Maybe these syssctls can be handled in a single sysctl-handler using arg2 for a switch statement? 
		
		if(sc->fif.fine_grain_ctrl) { // fan control via percentage
			SYSCTL_ADD_PROC(NULL, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
			"fan_speed", CTLTYPE_INT | CTLFLAG_RW, sc, 0,
			acpi_fan_level_sysctl, "I" ,"Fan speed in %");

			SYSCTL_ADD_INT(NULL, SYSCTL_CHILDREN(fan_oid), OID_AUTO, 
			"Step_size", CTLFLAG_RD, sc, 0, "Step size");
		}
		else {	// fan control via levels
			SYSCTL_ADD_PROC(NULL, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
			"current_fan_level", CTLTYPE_INT | CTLFLAG_RW, sc, 0,
			acpi_fan_level_sysctl, "I", "Fan level");
		
			// XXX: available fan levels, string? array?
			SYSCTL_ADD_INT(NULL, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
			"max_fan_levels", CTLFLAG_RD, sc, 0,"max fan levels");
		}
	
	
		if(acpi_fan_get_fst(dev))
			SYSCTL_ADD_PROC(NULL, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
			"rpm", CTLTYPE_INT | CTLFLAG_RD, sc, 0,
			acpi_fan_rpm_sysctl, "I" ,"current revolutions per minute"); // XXX: what is acpi_fan_rpm_sysctl? Why is this not an int readable?
	}	
	*/

	else {	/* acpi 1.0 */
		sc->acpi4 = 0;
		
		SYSCTL_ADD_PROC(NULL, SYSCTL_CHILDREN(fan_oid), OID_AUTO,
		"powered", CTLTYPE_INT | CTLFLAG_RW, sc, sc->fan_powered,
		acpi_fan_powered_sysctl, "I" ,"Fan OFF=0 ON=1 UNKNOWN=2");
	}	
	
	// XXX: Add a debug sysctl for testing!
	
	return 0;
}

static int
acpi_fan_detach(device_t dev) {
	
//	struct acpi_fan_softc *sc;
//    sc = device_get_softc(dev);

//	if(sc->acpi4)
//		AcpiOsFree(sc->fps);		/* remove the sysctls, dont change fan settings and leave. */
	return 0;
}

static int
acpi_fan_suspend(device_t dev) {
	//acpi_fan_set_power(dev, 0);				/* fan should be off in suspend mode, right? */
	return 0;
}

static int
acpi_fan_resume(device_t dev) {
	//acpi_fan_set_power(dev, 1);				/* turn fan on on resume. XXX: Do we need to check anything? */
	return 0;
}


/* Userland requestet fan level sysctl */
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

//	ACPI_SERIAL_BEGIN(fan);
	
    if(req->newptr) {	/* Write request */
		
		if(!sc->fan_powered)
			acpi_fan_set_power(dev, 1);	/* XXX: will this work? Do we need to sleep a bit? */

		SYSCTL_IN(req, &requested_speed, sizeof(requested_speed));
			
		if(sc->fif.fine_grain_ctrl) { /* fan is set via percentage: 0-100 % */
			
			/* check input */
			if((requested_speed <= 100) && (requested_speed >= 0)) {
				
				status = acpi_SetInteger(h, "_FSL", requested_speed);
				if (ACPI_FAILURE(status))
					ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
					"setting fan level: failed --%s\n", AcpiFormatException(status));
			}

			/* else: invalid input */
		}
		
		else {	/* fan is set via levels */ 
		
			if((requested_speed <= 100) && (requested_speed >= 0)) {	/* XXX: what is max fan level according to the spec? */
			
				status = acpi_SetInteger(h, "_FSL", requested_speed);
				if (ACPI_FAILURE(status))
					ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
					"setting fan level: failed --%s\n", AcpiFormatException(status));
			}
			
			/* else: invalid input */
		}
	}

    else /* read request */ {		
		acpi_fan_get_fst(dev); /* XXX: does it matter, whether it is fan level control or percentage level? */
		SYSCTL_OUT(req, &sc->fst.control, sizeof(sc->fst.control));
	}
	
//	ACPI_SERIAL_END(fan);
	
    return 0;
}

/* This sysctl controls if the fan is on or off. */
static int
acpi_fan_powered_sysctl(SYSCTL_HANDLER_ARGS) {
	
	struct acpi_fan_softc *sc;
	int error;
	
	sc = (struct acpi_fan_softc *) oidp->oid_arg1;

	if(!req->newptr) { /* Read request. */
		SYSCTL_OUT(req, &sc->fan_powered, sizeof(sc->fan_powered));
	}
	
	else { /* Write request. */
		error = SYSCTL_IN(req, &sc->fan_powered, sizeof(sc->fan_powered));
		if (error)
			return (error);

		/* Correct bogus writes to either 0 or 1. */
		if (sc->fan_powered !=0)
			sc->fan_powered = 1;
		
		/* Attempt to set the power state. */
		if (acpi_DeviceIsPresent(sc->dev)) {
			error = acpi_fan_get_power_state(sc->dev);
			if (error == 2) {
				/*XXX: My 1.0 compatible mainboard ends up here... */
				return (0);
			}
			if (error != sc->fan_powered)
				acpi_fan_set_power(sc->dev, sc->fan_powered);
		}
	}
	return (0);
}


/* This sysctl returns revolutions per minute */
static int acpi_fan_rpm_sysctl(SYSCTL_HANDLER_ARGS) {
	
	struct sysctl_oid *parent;
    struct acpi_fan_softc *sc;
    device_t dev;
//    ACPI_HANDLE h;
	long fan_index;

    parent = SYSCTL_PARENT(oidp); 
    fan_index = strtol(parent->oid_name, NULL, 0);
    dev = devclass_get_device(acpi_fan_devclass, (int) fan_index);

//    h = acpi_get_handle(dev);
    sc = device_get_softc(dev);	
	

    if(!req->newptr) {	/* read request */
		if(acpi_fan_get_fst(dev))
			SYSCTL_OUT(req, &sc->fst.speed, sizeof(sc->fst.speed));
		/* else error */
	}
    return 0;
}


static int acpi_fan_get_power_state(device_t dev) {

	ACPI_STATUS status;
	ACPI_HANDLE h;
	UINT32 state;
	

	h = acpi_get_handle(dev);

	/*
	* If no _STA method or if it failed, then assume that
	* it is ... Unknown (state=2)? Running (state=1)? 
	*/
//	ACPI_SERIAL_BEGIN(fan);
	
	status = acpi_GetInteger(h, "_STA",  &state);
	if(ACPI_FAILURE(status)) {
		ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev), 
		"Getting power status: failed --%s\n", AcpiFormatException(status));
		state = 2;
	}
	
//	ACPI_SERIAL_END(fan);
	
	return state;
}


/* This function turns the fan on and off. */
static void
acpi_fan_set_power(device_t dev, int new_state) {

	ACPI_HANDLE h;
	ACPI_STATUS status;

	h = acpi_get_handle(dev);

		if(new_state == 1) {
			// set fan to  D3 (On)
			//XXX: which one?
			//status = acpi_set_powerstate(dev, ACPI_STATE_D3); 
			//	status = acpi_pwr_switch_consumer(acpi_get_handle(dev), ACPI_STATE_D3);
			//status = AcpiEvaluateObject(h, "_PS3", NULL, NULL);
			status = AcpiEvaluateObject(h, "_ON", NULL, NULL);
			
						
			//status = AcpiEvaluateObject(h, "_PS3", NULL, NULL);
			if(ACPI_FAILURE(status)) 
				ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev), 
				"turning fan on: failed --%s\n", AcpiFormatException(status));
		}
	
		else if (new_state == 0) {
		//set fan to  D0 (Off)
			status = AcpiEvaluateObject(h, "_OFF", NULL, NULL);
			
			if(ACPI_FAILURE(status))
				ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
				"turning fan off: failed --%s\n", AcpiFormatException(status));
		}
		return;
}

static int acpi_fan_get_fif(device_t dev) {
	/*
    struct acpi_fan_softc *sc;
    ACPI_STATUS	as;
	ACPI_HANDLE	h;
	ACPI_HANDLE tmp;
	ACPI_BUFFER	fif_buffer;
	
	sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
	
	
	if(ACPI_FAILURE(acpi_GetHandleInScope(h, "_FIF", &tmp)))
		return 0;
	
	
	as = AcpiEvaluateObject(h, "_FIF", NULL, &fif_buffer);
    if (ACPI_FAILURE(as)) {
	ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
	    "error fetching: _FIF -- %s\n",
	    AcpiFormatException(as));
		return 0;
	}	
	memcpy(fif_buffer, &sc->fif, sizeof(fif_buffer)); */
	return 1;
}


static int acpi_fan_get_fst(device_t dev) {
	/*	
    struct acpi_fan_softc *sc;
    ACPI_STATUS	as;
	ACPI_HANDLE	h;
	ACPI_HANDLE tmp;
	ACPI_BUFFER	fst_buffer;
	
	sc = device_get_softc(dev);
    h = acpi_get_handle(dev);
	
	
	if(ACPI_FAILURE(acpi_GetHandleInScope(h, "_FST", &tmp)))
		return 0;
	
	as = AcpiEvaluateObject(h, "_FST", NULL, &fst_buffer);
    if (ACPI_FAILURE(as)) {
		ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
	    "error fetching: _FST -- %s\n",
	    AcpiFormatException(as));
		return 0;
	}
	memcpy(fst_buffer, &sc->fst, sizeof(*fst_buffer));
	// ACPIOsFree(fst_buffer); ??
	*/
	return 1;
}

static int acpi_fan_get_fps(device_t dev) {

/*
    struct acpi_cmbat_softc *sc;
	ACPI_HANDLE	handle;
	ACPI_BUFFER buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT *obj;
	ACPI_STATUS status;

	int i;
	
	sc = device_get_softc(dev);
	handle = acpi_get_handle(dev);
	
	if(ACPI_FAILURE(acpi_GetHandleInScope(handle, "_FST", &tmp)))
	return 0;
	
	status = acpi_evaluate_object(handle, "_FST", NULL, &buffer);
	if (ACPI_FAILURE(status))
		return 0;

	obj = buffer.pointer;
	if (!obj || obj->type != ACPI_TYPE_PACKAGE || obj->package.count < 2) {
		ACPI_VPRINT(dev, acpi_device_get_parent_softc(dev),
	    "error: invalid fps -- %s\n",
	    AcpiFormatException(as));
		//AcpiOsFree ??
		return 0;
	}

	sc->max_fps = obj->package.count - 1; */	/* minus revision field */
	
//	sc->acpi_fps = malloc(sizeof(obj), M_ACPIFAN, M_WAITOK); ???
//	sc->acpi_fan_fps = obj;

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