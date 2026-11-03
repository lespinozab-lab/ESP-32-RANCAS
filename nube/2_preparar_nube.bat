@echo off
rem conecta la pagina publica y los equipos con el proyecto de Supabase
rem pide la direccion del proyecto y su clave publica (estan en Supabase > Project Settings > API)
python "%~dp0preparar_nube.py"
if not errorlevel 1 start "" "%~dp0publicar"
pause
