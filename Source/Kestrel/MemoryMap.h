// MemoryMap.h -- where things are in the Kestrel16's 64 KB address space, as data.
//
// Definitions only, like Instructions.h: define the macro of the section you want, include,
// undefine. Source/Kestrel/Isa.h and Tools/definitions.py read it. No include guard on purpose.
//
// I/O is memory mapped from IoBase up; everything below it is RAM, which is where programs load.
// The I/O space is SlotCount slots of SlotSize bytes each: slot n starts at IoBase + n * SlotSize.
// Slot 0 is the system slot on the main board (the interrupt controller and the timer); slots
// 1-7 are connectors on the I/O backplane, each with its own interrupt line. I/O registers are
// words; a byte access reads or writes half of one, and a byte write to an odd address does
// nothing. An address no device answers reads 0.
//
//   Interrupt pending   read: bit n = slot n is requesting an interrupt (whether masked or not)
//   Interrupt mask      bit n = slot n may interrupt; 0 after reset. The CPU is interrupted while
//                       interrupts are enabled and (pending & mask) isn't 0.
//   Timer reload        write N: request an interrupt every N instructions (0 = off); read: N
//   Timer acknowledge   write anything: drop the timer's request (the handler must, or RETI
//                       re-enters it at once)
//   UART data           write: print the low byte; read: the next typed byte (0 if none)
//   UART status         bit ReceiveReady = a typed byte is waiting, bit TransmitReady = ready to
//                       print. The UART requests an interrupt while a byte is waiting.

#ifdef KestrelAddress
// KestrelAddress(Name, value)
KestrelAddress(ResetVector, 0x0000)          // PC after reset
    KestrelAddress(InterruptVector, 0x0002)  // where interrupts and TRAP go
    KestrelAddress(IoBase, 0xFC00)           // everything from here up is I/O
    KestrelAddress(StackTop, 0xFC00)         // where C programs start their stack (it grows down)
    KestrelAddress(DumpArea, 0xFBE0)         // KestrelValidator's __dump writes the registers here, last thing
#endif

#ifdef KestrelIoBus
    // KestrelIoBus(Name, value): the shape of the I/O space
    KestrelIoBus(SlotSize, 0x80)  // bytes per slot (address lines A0-A6 on a slot)
    KestrelIoBus(SlotCount, 8)
#endif

#ifdef KestrelSlot
    // KestrelSlot(Name, number): which slot each device sits in; its interrupt is pending bit number
    KestrelSlot(System, 0)
        KestrelSlot(Uart, 1)
#endif

#ifdef KestrelRegisterAddress
    // KestrelRegisterAddress(Name, address): the I/O registers
    KestrelRegisterAddress(InterruptPending, 0xFC00)
        KestrelRegisterAddress(InterruptMask, 0xFC02)
            KestrelRegisterAddress(TimerReload, 0xFC04)
                KestrelRegisterAddress(TimerAcknowledge, 0xFC06)
                    KestrelRegisterAddress(UartData, 0xFC80)
                        KestrelRegisterAddress(UartStatus, 0xFC82)
#endif

#ifdef KestrelUartStatusBit
    // KestrelUartStatusBit(Name, bit)
    KestrelUartStatusBit(ReceiveReady, 0)
        KestrelUartStatusBit(TransmitReady, 1)
#endif
