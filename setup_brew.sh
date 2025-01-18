#/bin/bash

brew_install() {
	printf "Installing homebrew..."
	which -s brew
	if [[ $? != 0 ]] ; then
		/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
	else
		printf " Already installed.\nUpdating and upgrading homebrew... "
		brew update && brew upgrade
	fi
}

brew_install_package() {
	printf "Installing $1..."
	if brew list $1 &>/dev/null; then
		printf " already installed.\n"
	else
		brew install $1 && printf "$1 is installed\n"
	fi
}

brew_install
brew_install_package "powershell/tap/powershell"

pwsh -f ./setup.ps1
