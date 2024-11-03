@echo off
mkdir build
pushd build
call cl -FC -nologo -Zi -Od ..\pmctrace_simple_test.cpp -Fepmctrace_simple_test_dm.exe
call cl -FC -nologo -Zi -O2 ..\pmctrace_simple_test.cpp -Fepmctrace_simple_test_rm.exe

where /q nasm || (echo WARNING: nasm not found -- threaded test will not be built)
call nasm -f win64 ..\pmctrace_test_asm.asm -o pmctrace_test_asm.obj
call lib -nologo pmctrace_test_asm.obj
call cl -FC -nologo -Zi -Od ..\pmctrace_threaded_test.cpp -Fepmctrace_threaded_test_dm.exe
call cl -FC -nologo -Zi -O2 ..\pmctrace_threaded_test.cpp -Fepmctrace_threaded_test_rm.exe

popd build
