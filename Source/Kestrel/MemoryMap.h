// MemoryMap.h -- where things are in the Kestrel16's 64 KB address space, as data.
//
// Definitions only, like Instructions.h: define the macro of the section you want, include,
// undefine. Source/Kestrel/Isa.h and Tools/definitions.py read it. No include guard on purpose.
//
// I/O is memory mapped from IoBase up; everything below it is RAM, which is where programs load.
// I/O registers are words; a byte access reads or writes half of one.
//
//   UART data     write: print the low byte; read: the next typed byte (0 if none)
//   UART status   bit ReceiveReady = a typed byte is waiting, bit TransmitReady = ready to print
//   Timer         write N: raise an interrupt every N instructions (0 = off); read: N

#ifdef KestrelAddress
// KestrelAddress(Name, value)
KestrelAddress(ResetVector, 0x0000)        // PC after reset
KestrelAddress(InterruptVector, 0x0002)    // where interrupts and TRAP go
KestrelAddress(IoBase, 0xFF00)             // everything from here up is I/O
KestrelAddress(StackTop, 0xFF00)           // where C programs start their stack (it grows down)
#endif

#ifdef KestrelDevice
// KestrelDevice(Name, base, size): each device's address range, in bytes
KestrelDevice(Uart, 0xFF00, 4)
KestrelDevice(Timer, 0xFF04, 4)
#endif

#ifdef KestrelRegisterAddress
// KestrelRegisterAddress(Name, address): the I/O registers
KestrelRegisterAddress(UartData, 0xFF00)
KestrelRegisterAddress(UartStatus, 0xFF02)
KestrelRegisterAddress(TimerReload, 0xFF04)
#endif

#ifdef KestrelUartStatusBit
// KestrelUartStatusBit(Name, bit)
KestrelUartStatusBit(ReceiveReady, 0)
KestrelUartStatusBit(TransmitReady, 1)
#endif
