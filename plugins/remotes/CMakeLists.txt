# Remote bookmarks (smb://, sftp://) mounted through gvfs over D-Bus. Needs sdbus-c++ to build
# (Nordend has it anyway for udisks2) and gvfs at run time; without gvfs its bookmarks say so.
nordend_add_plugin(remotes
    SUMMARY "smb:// and sftp:// bookmarks, mounted through gvfs"
    SOURCES src/gvfs.cpp src/remotes.cpp
    LINK nordend::platform PkgConfig::SDBUSCPP)
