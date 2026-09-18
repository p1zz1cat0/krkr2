// Weak fallbacks for internal plugin anchors that are not present in every
// link of core_plugin_module.
//
// PluginImpl.cpp calls TVPWindowMessageChannelPluginAnchor(), and the plugin
// itself lives in krkr2plugin.  The app links both, but the core unit tests
// link krkr2core alone, so without a fallback those executables fail to link.
// The fallback must live in its own translation unit: a weak *definition* in
// the same object file as the call resolves locally and the linker then never
// pulls the real anchor out of krkr2plugin (verified with the app binary --
// the plugin registered nothing).  A weak definition in a separate object file
// is still overridden by the strong definition in the archive.
//
// Keep this list to test-only internal plugins.  Game-facing plugins are either
// part of core or always linked with krkr2plugin, so they need no fallback.

extern "C" __attribute__((weak)) void TVPWindowMessageChannelPluginAnchor() {}
