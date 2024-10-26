#  FreeBSD acpi fan driver ("PNP0C0B")

Work in progress :- )

This is only a skeleton driver. Use it for your inspiration.

I have no hardware to test this. :-)

Have a nice day :-)

Steps:
1. Add the file acpi_fan.c to the directory: /usr/src/sys/dev/acpica/
2. Add the word "fan" to the file: /usr/src/sys/modules/acpi/acpi/Makefile
3. Add the line "dev/acpica/acpi_cmbat.c		optional acpi" to the file: /usr/src/sys/conf/files
4. Now you can compile your kernel, e.g. with the option MODULES_OVERRIDE=acpi
5. Edit the acpi_fan.c skeleton file so that it actually does something. 
