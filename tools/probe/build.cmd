@echo off
rem Build probe.exe. Run from a VS developer prompt (needs cl.exe + Windows SDK).
rem The C++/WinRT projection headers come from the main project's build output
rem ("Generated Files"), so build the main project once first.
rem
rem   vcvars64.bat && cl /nologo /utf-8 /EHsc /std:c++latest /await /W3 /D_UNICODE /DUNICODE ^
rem     /Fe:probe.exe probe.cpp /link /SUBSYSTEM:CONSOLE windowsapp.lib ole32.lib
rem
rem (INCLUDE must also contain the main project's "Generated Files" directory.)
cl /nologo /utf-8 /EHsc /std:c++latest /await /W3 /D_UNICODE /DUNICODE /Fe:probe.exe probe.cpp /link /SUBSYSTEM:CONSOLE windowsapp.lib ole32.lib
