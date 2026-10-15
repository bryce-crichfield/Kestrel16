"""The machine's definitions, read from the C++ headers that hold them, so the generators and the
C++ side can't drift apart:

    Source/Kestrel/Instructions.h    the instruction set
    Source/Kestrel/MemoryMap.h       the address map
    Source/Kestrel/AluOperations.h   the hardware ALU's operation codes

Each line of those files that isn't a comment or a # directive is a macro call, Kestrel<Kind>(...),
whose arguments are names, numbers or "quoted text". The tables here are keyed by name:

    Opcodes['System'] == 12, Conditions['Equal'] == 1, Devices['Timer'] == (0xFF04, 4), ...
"""
import os
import re

SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'Source', 'Kestrel')


def value(text):
    """One macro argument: a number, "text" (without the quotes) or a name."""
    if text.startswith('"') and text.endswith('"'):
        return text[1:-1]
    if re.fullmatch(r'0[xX][0-9a-fA-F]+|\d+', text):
        return int(text, 0)
    return text


def read(fileName):
    """Every macro call in a definitions header, as {kind: [arguments, ...]} in file order."""
    calls = {}
    with open(os.path.join(SOURCE, fileName), encoding='utf-8') as file:
        for line in file:
            line = line.split('//')[0].strip()
            match = re.fullmatch(r'Kestrel(\w+)\((.*)\)', line)
            if match:
                arguments = [value(argument.strip()) for argument in match.group(2).split(',')]
                calls.setdefault(match.group(1), []).append(arguments)
    return calls


instructions, memoryMap, aluOperations = read('Instructions.h'), read('MemoryMap.h'), read('AluOperations.h')

Constants = {name: number for name, number in instructions['Constant']}
Fields = {name: (shift, bits) for name, shift, bits in instructions['Field']}  # shift, bits
Opcodes = {name: number for name, number in instructions['Opcode']}
AluFunctions = {name: number for name, number in instructions['AluFunction']}
Alu2Functions = {name: number for name, number in instructions['Alu2Function']}
SystemFunctions = {name: number for name, number in instructions['SystemFunction']}
Conditions = {name: number for name, number, suffix in instructions['Condition']}

Addresses = {name: number for name, number in memoryMap['Address']}
Devices = {name: (base, size) for name, base, size in memoryMap['Device']}  # base, size in bytes
RegisterAddresses = {name: number for name, number in memoryMap['RegisterAddress']}

AluOperations = {name: code for name, code in aluOperations['AluOperation']}
