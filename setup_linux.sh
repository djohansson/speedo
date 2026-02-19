if ! command -v pwsh &> /dev/null
then
	echo "pwsh could not be found, installing to ~/powershell"
	
	sudo apt-get update
	sudo apt-get install curl wget jq libunwind8 -y

	bits=$(getconf LONG_BIT)
	release=$(curl -sL https://api.github.com/repos/PowerShell/PowerShell/releases/latest)
	package=$(echo $release | jq -r ".assets[].browser_download_url" | grep "linux-arm${bits}.tar.gz")

	wget $package

	mkdir ~/powershell

	tar -xvf "./${package##*/}" -C ~/powershell
	chmod 755 ~/powershell/pwsh
	rm "./${package##*/}"

	sudo ~/powershell/pwsh -command 'New-Item -ItemType SymbolicLink -Path "/usr/bin/pwsh" -Target "$PSHOME/pwsh" -Force'
fi

pwsh -f ./setup.ps1