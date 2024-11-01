@echo off
mkdir build
pushd build
call nasm -f win64 ..\pmctrace_test_asm.asm -o pmctrace_test_asm.obj
call lib -nologo pmctrace_test_asm.obj
call cl -FC -nologo -Zi -Od ..\pmctrace_test.cpp -Fepmctrace_test_dm.exe
call cl -FC -nologo -Zi -O2 ..\pmctrace_test.cpp -Fepmctrace_test_rm.exe
popd build
