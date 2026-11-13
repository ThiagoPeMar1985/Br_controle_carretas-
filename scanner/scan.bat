@echo off
rem Duplo clique: relatorio dos enderecos quebrados no exe atual
rem   scan.bat --apply           reescreve os VA movidos na tabela
rem   scan.bat --apply --ender-md  tambem atualiza o ENDERECOS.md
cd /d "%~dp0"
python scan.py %*
pause
