# freebsd_acpi_fan
Work in progress - skeleton acpi fan driver

This is a work in progress! What it currently does:

   - driver attaches to the fan
   - it checks for acpi version 0 and acpi version 4.0.

The actual acpi version 4.0 fan control needs to be programmed still.
I prefer sysctls, but an alternative would be control via /dev/acpi.
I have no fan device for testing. Once I find one, I will finish this project. I never compiled the code. It will have bugs.
The code just gives an idea of how you could start off.. :-)
