# Install and use

Requires Bitwig Studio 6.1.7.24824 for Windows x64 AVX2, Patchbay 0.36.0-dev.14 with Actions and Menus, Notifications and Panels, and Melodyne VST3. Add the mod ZIP in Patchbay and launch Bitwig from Patchbay.

With the audio engine active, right-click an Arranger audio event backed by a local mono or stereo WAV and choose **Edit with Melodyne…**. For a clip with several events, select the event in the detail editor. If several clips share the same event, open an independent clip in Melodyne. Separate events can use the same source WAV. Changes play automatically. If there are pending Melodyne edits, **Export Audio** waits for a rendered WAV and attaches it to the event before starting.

While **Export Audio** is rendering, you can keep editing in Melodyne. BwARA applies further edits in Bitwig and handles queued saves once the export finishes or is canceled.

**Ctrl+S** in either window saves the edit. Bitwig's project-close prompt handles unsaved changes and closes Melodyne when the project closes. Keep the project media when moving the project. Reopen the editor from the same event to continue editing.

An applied Melodyne edit appears in Bitwig's **Edit** menu as **Apply Melodyne Edit**. Undo or redo it there to restore the audio, waveform, and notes in the open Melodyne window, even after reopening Melodyne. You can undo all the way back to the original source. Keyboard shortcuts go to the focused window.

Undoing a deletion restores the event with its latest applied Melodyne edit. Reopen Melodyne on that event to continue editing. Redoing the deletion closes its Melodyne window.

You can move or trim the bound clip or audio event while the editor is open, provided it still contains the same event and source. You can also undo and redo a move along the same track in Bitwig while keeping the Melodyne edit active. Deleting the bound event or its clip, or replacing the source, closes Melodyne. If the event becomes shared while editing, BwARA closes Melodyne before publishing or saving another edit.

Splitting an independent Arranger clip or an audio event in the detail editor carries the latest applied edit into both parts. The open editor stays with the left part. Close it to open and edit the right part independently. Undoing the split closes the right part's editor when that part disappears.

Ordinary copy and paste carries the latest applied audio and Melodyne state into an independent copy. You can keep editing the original while the copy keeps its version. Save first if a recent edit is still rendering.

Melodyne follows Bitwig's transport. Use Space to start or stop Bitwig, or double-click the Melodyne ruler to play from that position. With Bitwig stopped, double-click the note editor background to audition through the Windows default output.

In a single Bitwig window, select **Melodyne** to dock the open editor. Switching tabs returns it to a resizable floating window behind Bitwig. Use Alt+Tab or the taskbar to bring it forward.

Live preview files are recycled during editing. Error sessions are retained for seven days and reclaimed on a later launch of the editor. Each applied edit leaves a WAV snapshot in the project media for undo and older project saves.
