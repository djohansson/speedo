Remove-Item $PSScriptRoot/.env.json -ErrorAction Ignore
Remove-Item $PSScriptRoot/*.fdb -ErrorAction Ignore
Remove-Item $PSScriptRoot/build/* -Recurse -ErrorAction Ignore
#Remove-Item "$HOME/.vcpkg" -Recurse -ErrorAction Ignore
