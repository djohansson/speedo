#/bin/zsh

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

#curl -O https://sdk.lunarg.com/sdk/download/latest/mac/vulkan_sdk.dmg
#hdiutil attach vulkan_sdk.dmg
#sudo ./InstallVulkan.app/Contents/MacOS/InstallVulkan --root $(mkdir build.vulkan; cd ./build.vulkan ; pwd) --accept-licenses --default-answer --confirm-command install
#hdiutil detach /Volumes/vulkansdk-macos-*
#rm vulkan_sdk.dmg
#sudo python3 ~/VulkanSDK/*/install_vulkan.py --install-json-location ~/VulkanSDK/*/

pwsh -f ./setup.ps1
