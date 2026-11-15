# Install and use

Requires Bitwig Studio 6.1.7.24824 for Windows x64 AVX2, Patchbay 0.36.0-dev.14 with Actions and Menus, Notifications and Panels, and Melodyne VST3. Add the mod ZIP in Patchbay and launch Bitwig from Patchbay.

With the audio engine active, right-click an Arranger audio event backed by a local mono or stereo WAV and choose **Edit with Melodyne…**. For a clip with several events, select the event in the detail editor. Changes play automatically and are included in exports.

**Ctrl+S** in either window saves the edit. Bitwig's project-close prompt handles unsaved changes and closes Melodyne when the project closes. Keep the project media when moving the project. Reopen the editor from the same event to continue editing.

You can move or trim the bound clip or audio event while the editor is open, provided it still contains the same event and source. Deleting the bound event or its clip, or replacing the source, closes Melodyne. Save before deleting or replacing audio to keep the latest edits available if you undo the operation. Close Melodyne before splitting, copying, or undoing changes to the event.

Melodyne follows Bitwig's transport. Use Space to start or stop Bitwig, or double-click the Melodyne ruler to play from that position. With Bitwig stopped, double-click the note editor background to audition through the Windows default output.

In a single Bitwig window, the experimental **Melodyne** panel docks the open editor when its tab is visible and large enough. Switch to another tab to float it again.

Live preview files are recycled during editing. Error sessions are retained for seven days and reclaimed on a later launch of the editor. Project media remains available for undo and older project saves.
