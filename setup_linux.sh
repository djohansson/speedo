#!/bin/bash
set -e

# Installs or upgrades PowerShell 7.5+ (to ~/powershell) if needed, then runs setup.ps1 with it.

pwsh_ok() {
	"$1" -NoProfile -Command 'exit [int]($PSVersionTable.PSVersion -lt [version]"7.5")' &>/dev/null
}

pwsh=pwsh
if ! pwsh_ok pwsh; then
	echo "PowerShell 7.5+ could not be found, installing latest PowerShell to ~/powershell"

	case "$(uname -m)" in
		x86_64|amd64) arch=x64 ;;
		aarch64|arm64) arch=arm64 ;;
		*) echo "Unsupported architecture: $(uname -m)"; exit 1 ;;
	esac

	sudo apt-get update
	sudo apt-get install curl libunwind8 -y

	# resolve the latest version from the releases/latest redirect: the GitHub API rate limits unauthenticated
	# requests per IP (60/h), which shared IPs (offices, CI) exhaust
	tag=$(curl -fsSL -o /dev/null -w '%{url_effective}' https://github.com/PowerShell/PowerShell/releases/latest)
	tag=${tag##*/}
	package="https://github.com/PowerShell/PowerShell/releases/download/${tag}/powershell-${tag#v}-linux-${arch}.tar.gz"

	tmp=$(mktemp -d)
	curl -fsSL -o "$tmp/powershell.tar.gz" "$package"

	# replace any previous install rather than extracting over it
	rm -rf ~/powershell
	mkdir -p ~/powershell
	tar -xzf "$tmp/powershell.tar.gz" -C ~/powershell
	chmod 755 ~/powershell/pwsh
	rm -rf "$tmp"

	# /usr/local/bin precedes /usr/bin on PATH, so this also shadows an older distro-packaged pwsh without clobbering it
	sudo ln -sf ~/powershell/pwsh /usr/local/bin/pwsh

	pwsh=~/powershell/pwsh
	if ! pwsh_ok "$pwsh"; then
		echo "PowerShell 7.5+ installation failed"
		exit 1
	fi
fi

"$pwsh" -f "$(dirname "$0")/setup.ps1" "$@"
