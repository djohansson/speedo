#!/bin/bash
set -e

# Installs or upgrades Homebrew and PowerShell 7.5+ if needed, then runs setup.ps1 with it.

brew_install() {
	printf "Installing homebrew..."
	if ! which -s brew; then
		/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

		# a fresh install on Apple Silicon isn't on PATH yet
		if [[ -x /opt/homebrew/bin/brew ]]; then
			eval "$(/opt/homebrew/bin/brew shellenv)"
		fi
	else
		printf " Already installed.\nUpdating and upgrading homebrew... "
		brew update && brew upgrade
	fi
}

pwsh_ok() {
	"$1" -NoProfile -Command 'exit [int]($PSVersionTable.PSVersion -lt [version]"7.5")' &>/dev/null
}

brew_install

if ! pwsh_ok pwsh; then
	echo "PowerShell 7.5+ could not be found, installing latest PowerShell..."

	# the powershell/tap formula is deprecated and no longer updated, the cask is the supported package
	if brew list --formula powershell/tap/powershell &>/dev/null; then
		brew uninstall --formula powershell/tap/powershell
	fi

	if brew list --cask powershell &>/dev/null; then
		brew upgrade --cask powershell
	else
		brew install --cask powershell
	fi

	hash -r
	if ! pwsh_ok pwsh; then
		echo "PowerShell 7.5+ installation failed"
		exit 1
	fi
fi

pwsh -f "$(dirname "$0")/setup.ps1" "$@"
