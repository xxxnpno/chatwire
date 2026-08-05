# A DLL that is mapped into a running game cannot be overwritten -- but on
# Windows it CAN be renamed, because renaming touches the directory entry rather
# than the mapped pages.  chatwire deliberately stays mapped after a detach (see
# dllmain.cpp), so a rebuild while the game is open would otherwise fail the link
# with "Permission denied".  Moving the old file aside costs nothing and lets the
# link proceed; the stale copies are harmless and swept below.
if(EXISTS "${DLL}")
    string(RANDOM LENGTH 8 ALPHABET "0123456789abcdef" SUFFIX)
    file(RENAME "${DLL}" "${DLL}.locked-${SUFFIX}" RESULT ignored)
endif()

# Sweep anything left from previous builds that the OS has since released.
file(GLOB stale "${DLL}.locked-*")
foreach(old IN LISTS stale)
    file(REMOVE "${old}")
endforeach()
