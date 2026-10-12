# runs after every x64-windows-clang portfile (VCPKG_POST_PORTFILE_INCLUDES): copies each dll's pdb next to it with the
# triplet's vcpkg_copy_pdbs, for the ports that don't call it themselves. copying one again is harmless
vcpkg_copy_pdbs()
