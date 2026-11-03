@echo off
rem baja lo guardado en el equipo (CSV + Excel) a la carpeta datos y la abre
rem la laptop debe estar en el mismo wifi que el equipo
python "%~dp0extraer_datos.py"
if not errorlevel 1 start "" "%~dp0..\..\datos\esp32-csv"
pause
