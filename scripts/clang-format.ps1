. $PSScriptRoot/env.ps1

Initialize-SystemEnv

(Get-ChildItem -Path $PSScriptRoot/../src -Include *.h,*.c,*.cpp -Recurs -File).FullName | Out-File -FilePath $PSScriptRoot/../.clang-format-files -Force

clang-format -i -style=file -files $PSScriptRoot/../.clang-format-files