#  FreeBSD acpi fan driver ("PNP0C0B")

Work in progress :- )

This is only a skeleton driver. Use it for your inspiration.

I have no hardware to test this. :-)

Have a nice day :-)

Steps:
1. Add the file acpi_fan.c to the directory: /usr/src/sys/dev/acpica/
2. Add the line "dev/acpica/acpi_fan.c		optional acpi" to the file: /usr/src/sys/conf/files
3. Now you can compile and install your kernel. It will have acpi fan device.
4. Edit the acpi_fan.c skeleton file so that it actually does something. 
