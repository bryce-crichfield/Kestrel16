#include "Machine.h"

#include "Alu.h"

namespace Kestrel {

namespace {

constexpr uint16_t WordAlignMask = 0xFFFE, ByteMask = 0xFF;
constexpr uint8_t TimerBit = 1 << Slot::System, UartBit = 1 << Slot::Uart;
constexpr unsigned ShiftAmountMask = 15;

}  // namespace

// ---------------------------------------------------------------------------
// Memory and I/O

uint16_t Machine::IoRead(uint16_t address)
{
    switch (address & WordAlignMask) {
    case Address::InterruptPending:
        return PendingInterrupts(0xFF);
    case Address::InterruptMask:
        return cpu.interruptMask;
    case Address::UartData:
        return terminal.KeyWaiting() ? terminal.ReadKey() : 0;
    case Address::UartStatus:
        return static_cast<uint16_t>((terminal.KeyWaiting() ? UartStatus::ReceiveReady : 0) | UartStatus::TransmitReady);
    case Address::TimerReload:
        return cpu.timerReload;
    default:
        return 0;
    }
}

void Machine::IoWrite(uint16_t address, uint16_t value)
{
    switch (address & WordAlignMask) {
    case Address::InterruptMask:
        cpu.interruptMask = static_cast<uint8_t>(value & ByteMask);
        break;
    case Address::TimerAcknowledge:
        cpu.timerPending = false;
        break;
    case Address::UartData:
        terminal.Print(static_cast<uint8_t>(value & ByteMask));
        break;
    case Address::TimerReload:
        cpu.timerReload = value;
        cpu.timerCount = value;
        break;
    default:
        break;
    }
}

uint16_t Machine::ReadMemoryWord(uint16_t address) const
{
    address &= WordAlignMask;
    return static_cast<uint16_t>(memory[address] | (memory[address + 1] << 8));
}

uint16_t Machine::ReadWord(uint16_t address)
{
    address &= WordAlignMask;
    if (address >= Address::IoBase) return IoRead(address);
    return ReadMemoryWord(address);
}

void Machine::WriteWord(uint16_t address, uint16_t value)
{
    address &= WordAlignMask;
    if (address >= Address::IoBase) {
        IoWrite(address, value);
        return;
    }
    memory[address] = static_cast<uint8_t>(value & ByteMask);
    memory[address + 1] = static_cast<uint8_t>(value >> 8);
}

uint16_t Machine::ReadByte(uint16_t address)
{
    if (address >= Address::IoBase) {
        uint16_t value = IoRead(address);
        return (address & 1) ? static_cast<uint16_t>(value >> 8) : static_cast<uint16_t>(value & ByteMask);
    }
    return memory[address];
}

void Machine::WriteByte(uint16_t address, uint8_t value)
{
    if (address >= Address::IoBase) {
        if (!(address & 1)) IoWrite(address, value);
        return;
    }
    memory[address] = value;
}

// ---------------------------------------------------------------------------
// CPU core

void Machine::TickTimer()
{
    if (cpu.timerReload && --cpu.timerCount == 0) {
        cpu.timerPending = true;
        cpu.timerCount = cpu.timerReload;
    }
}

uint8_t Machine::PendingInterrupts(uint8_t slots)
{
    uint8_t pending = cpu.timerPending ? TimerBit : 0;
    if ((slots & UartBit) && terminal.KeyWaiting()) pending |= UartBit;  // asks the terminal only if it matters
    return static_cast<uint8_t>(pending & slots);
}

bool Machine::CanWake()
{
    if (!cpu.interruptsEnabled) return false;
    return ((cpu.interruptMask & TimerBit) && (cpu.timerReload || cpu.timerPending)) || (cpu.interruptMask & UartBit);
}

uint8_t Machine::PackFlags() const
{
    return static_cast<uint8_t>((cpu.flags & Flag::ArithmeticMask) | (cpu.interruptsEnabled ? Flag::InterruptEnable : 0));
}

void Machine::EnterException(uint16_t returnAddress)
{
    cpu.exceptionProgramCounter = returnAddress;
    cpu.exceptionFlags = PackFlags();
    cpu.interruptsEnabled = false;
}

// Returns false for an illegal function code.
bool Machine::ExecuteAlu(const Instruction& in)
{
    uint16_t a = cpu.registers[in.sourceA], b = cpu.registers[in.sourceB], result;
    unsigned shiftAmount = b & ShiftAmountMask;

    switch (static_cast<AluFunction>(in.function)) {
    case AluFunction::Add:                  result = AluAdd(a, b, 0, cpu.flags); break;
    case AluFunction::Subtract:             result = AluSubtract(a, b, 1, cpu.flags); break;
    case AluFunction::And:                  result = AluLogic(a & b, cpu.flags); break;
    case AluFunction::Or:                   result = AluLogic(a | b, cpu.flags); break;
    case AluFunction::Xor:                  result = AluLogic(a ^ b, cpu.flags); break;
    case AluFunction::ShiftLeft:            result = AluShift(ShiftKind::Left, a, shiftAmount, cpu.flags); break;
    case AluFunction::ShiftRightLogical:    result = AluShift(ShiftKind::RightLogical, a, shiftAmount, cpu.flags); break;
    case AluFunction::ShiftRightArithmetic: result = AluShift(ShiftKind::RightArithmetic, a, shiftAmount, cpu.flags); break;
    default:                                return false;
    }
    cpu.registers[in.destination] = result;
    return true;
}

// Returns false for an illegal function code.
bool Machine::ExecuteAlu2(const Instruction& in)
{
    uint16_t a = cpu.registers[in.sourceA], b = cpu.registers[in.sourceB];
    unsigned carryIn = (cpu.flags & Flag::Carry) != 0;
    uint16_t& destination = cpu.registers[in.destination];

    switch (static_cast<Alu2Function>(in.function)) {
    case Alu2Function::AddWithCarry:      destination = AluAdd(a, b, carryIn, cpu.flags); break;
    case Alu2Function::SubtractWithCarry: destination = AluSubtract(a, b, carryIn, cpu.flags); break;
    case Alu2Function::Compare:           AluSubtract(a, b, 1, cpu.flags); break;
    case Alu2Function::Move:              destination = a; break;
    case Alu2Function::Not:               destination = AluLogic(static_cast<uint16_t>(~a), cpu.flags); break;
    case Alu2Function::Negate:            destination = AluSubtract(0, a, 1, cpu.flags); break;
    case Alu2Function::SignExtendByte:    destination = AluLogic(SignExtendByte(a), cpu.flags); break;
    default:                              return false;
    }
    return true;
}

// Updates nextAddress for control transfers. Returns false for an illegal function code.
bool Machine::ExecuteSystem(const Instruction& in, uint16_t& nextAddress)
{
    switch (static_cast<SystemFunction>(in.systemFunction)) {
    case SystemFunction::JumpRegister:
        nextAddress = cpu.registers[in.systemSource];
        break;
    case SystemFunction::JumpAndLinkRegister: {
        uint16_t target = cpu.registers[in.systemSource];
        cpu.registers[LinkRegister] = nextAddress;
        nextAddress = target;
        break;
    }
    case SystemFunction::ReturnFromInterrupt:
        cpu.flags = cpu.exceptionFlags & Flag::ArithmeticMask;
        cpu.interruptsEnabled = (cpu.exceptionFlags & Flag::InterruptEnable) != 0;
        nextAddress = cpu.exceptionProgramCounter;
        break;
    case SystemFunction::EnableInterrupts:  cpu.interruptsEnabled = true; break;
    case SystemFunction::DisableInterrupts: cpu.interruptsEnabled = false; break;
    case SystemFunction::Halt:              cpu.halted = true; break;
    case SystemFunction::Trap:
        EnterException(nextAddress);
        nextAddress = Address::InterruptVector;
        break;
    default:
        return false;
    }
    return true;
}

uint16_t Machine::EffectiveAddress(const Instruction& in) const
{
    return static_cast<uint16_t>(cpu.registers[in.sourceA] + in.immediate6);
}

StepResult Machine::Step(StepInfo* info)
{
    cpu.steps++;
    TickTimer();

    // Requests are levels: taking one doesn't clear it, the handler does (at the device).
    if (cpu.interruptsEnabled && PendingInterrupts(cpu.interruptMask)) {
        cpu.halted = false;
        EnterException(cpu.programCounter);
        cpu.programCounter = Address::InterruptVector;
    }

    if (cpu.halted) return CanWake() ? StepResult::Running : StepResult::Halted;

    uint16_t address = cpu.programCounter & WordAlignMask;
    uint16_t word = ReadMemoryWord(address);
    uint16_t nextAddress = static_cast<uint16_t>(address + InstructionSize);
    Instruction in = Decode(word);
    bool legal = true;
    if (info) *info = {true, address, word};

    switch (in.opcode) {
    case Opcode::Alu:
        legal = ExecuteAlu(in);
        break;
    case Opcode::Alu2:
        legal = ExecuteAlu2(in);
        break;
    case Opcode::AddImmediate:
        cpu.registers[in.destination] = AluAdd(cpu.registers[in.sourceA], static_cast<uint16_t>(in.immediate6), 0, cpu.flags);
        break;
    case Opcode::LoadImmediate:
        cpu.registers[in.destination] = static_cast<uint16_t>(in.immediate8);
        break;
    case Opcode::LoadHighImmediate:
        cpu.registers[in.destination] = static_cast<uint16_t>((cpu.registers[in.destination] & ByteMask) | (in.immediate8 << 8));
        break;
    case Opcode::LoadWord:
        cpu.registers[in.destination] = ReadWord(EffectiveAddress(in));
        break;
    case Opcode::StoreWord:
        WriteWord(EffectiveAddress(in), cpu.registers[in.destination]);
        break;
    case Opcode::LoadByte:
        cpu.registers[in.destination] = ReadByte(EffectiveAddress(in));
        break;
    case Opcode::StoreByte:
        WriteByte(EffectiveAddress(in), static_cast<uint8_t>(cpu.registers[in.destination] & ByteMask));
        break;
    case Opcode::Branch:
        if (ConditionHolds(in.condition, cpu.flags)) nextAddress = BranchTarget(nextAddress, in.offset8);
        break;
    case Opcode::Jump:
        nextAddress = BranchTarget(nextAddress, in.offset12);
        break;
    case Opcode::JumpAndLink:
        cpu.registers[LinkRegister] = nextAddress;
        nextAddress = BranchTarget(nextAddress, in.offset12);
        break;
    case Opcode::System:
        legal = ExecuteSystem(in, nextAddress);
        break;
    case Opcode::CompareImmediate:
        AluSubtract(cpu.registers[in.sourceA], static_cast<uint16_t>(in.immediate6), 1, cpu.flags);
        break;
    default:
        legal = false;
        break;
    }

    if (!legal) {
        cpu.programCounter = address;
        return StepResult::Illegal;
    }
    cpu.programCounter = nextAddress & WordAlignMask;
    return StepResult::Running;
}

}  // namespace Kestrel
