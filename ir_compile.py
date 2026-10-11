#!/usr/bin/env python3
"""Compile LLVM IR (.ll) to an object file (.o) using llvmlite."""
import sys
import llvmlite.binding as llvm

def main():
    if len(sys.argv) != 3:
        print("Usage: ir_compile.py <input.ll> <output.o>", file=sys.stderr)
        sys.exit(1)

    llvm.initialize_all_targets()
    llvm.initialize_all_asmprinters()

    with open(sys.argv[1], 'r') as f:
        ir_text = f.read()

    mod = llvm.parse_assembly(ir_text)
    mod.verify()

    target = llvm.Target.from_default_triple()
    tm = target.create_target_machine(opt=2)
    obj = tm.emit_object(mod)

    with open(sys.argv[2], 'wb') as f:
        f.write(obj)

    print(f"Compiled {sys.argv[1]} -> {sys.argv[2]} ({len(obj)} bytes)")

if __name__ == '__main__':
    main()
