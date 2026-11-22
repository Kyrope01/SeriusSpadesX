@echo off
rem The paks rebuilt in one command or one double click, whatever the
rem machine's script policy says: the policy is bypassed for this call
rem only, the system is left as it was.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rebuild-paks.ps1" %*
