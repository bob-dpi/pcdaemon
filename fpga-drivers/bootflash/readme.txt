```

HARDWARE
   This peripheral provides read and write access to the
flash memory used to boot the FPGA.  A boot flash allows
the peripheral controller card to be usable within a few
seconds after power up.
   You can load a PCCore file to the flash and you can read
the contents of the boot flash into a file.  Both operations
take more than a few seconds.  
   The size of the flash device is part of the 'info' data.


RESOURCES
The 'file' resource lets you read and write files to the
flash, and the 'info' resource display JEDEC information
regarding the device.

file: PCCore.bin file to load into, or read from the boot
flash.  When reading a file you can optionally specify
how many 64K blocks to read.  When writing you may need
to specify the full path to the your source file.

info: Manufacture Id, Device ID, and size in bytes


EXAMPLES:
    # display information about the flash device
    pcget bootflash info
    # write an FPGA configuration file to flash
    pcset bootflash file /var/local/pcdaemon/PCCore.bin
    # read 16 64K block (1MB) to a file
    pcget bootflash file /tmp/verify.bin 16


NOTES:
  As a daemon, pcdaemon does not print messages to the
console.  If you are going to be writing files to the
flash you may want to start pcdaemon with the -efd
flags.  This will give status messages about the flash
on the console.

  Winbond has a manufacturer ID of 0xEF.  The Winbond
device W25Q128JV-IM (or-JM) has a device ID of 0x70.

```
