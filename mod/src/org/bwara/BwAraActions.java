package org.bwara;

import bitwig.obf.DyF; // 编曲区片段实例
import bitwig.obf.bju; // 时间线选区
import bitwig.obf.drf; // Bitwig 的 UI 事件循环
import bitwig.obf.eOj; // 编曲区片段容器
import bitwig.obf.er; // 把工程位置映射到片段内部，包含偏移和循环
import bitwig.obf.poA; // 片段内的音频事件
import bml.actions.ActionContext;
import bml.actions.ActionProvider;
import bml.actions.Actions;
import bml.actions.Menu;
import bml.api.Bml;
import bml.notifications.Notifications;
import bml.notifications.Severity;
import com.bitwig.base.async.Pv;
import com.bitwig.base.io.Lpf; // 素材库文件位置
import com.bitwig.flt.app.gEH;
import com.bitwig.flt.document.core.master.CmH; // 工程文档
import com.bitwig.flt.document.core.master.dEy; // 撤销事务
import com.bitwig.flt.document.core.master.fRJ; // 音频素材
import com.bitwig.flt.document.core.master.glu; // 事件自己的素材引用

import java.io.IOException;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.NoSuchFileException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.stream.Stream;

public final class BwAraActions implements ActionProvider {
    private static final Bml.Mod MOD = Bml.mod("bwara");
    // active 要等子进程启动后才存在；这把锁连启动阶段也管住，防止双击开出两桌。
    private static final AtomicBoolean EDITING = new AtomicBoolean();
    private static volatile LiveSelection active;
    private static final String EDITED = "edited.wav";
    private static final String SOURCE = "source.wav";
    private static final String ARCHIVE = "edit.ara";
    private static final String LOG = "host.log";
    private static final long PID = ProcessHandle.current().pid();

    @Override
    public void declare(Actions actions) {
        actions.action("bwara_edit_region", "Edit with Melodyne")
            .menuLabel("Edit with Melodyne…")
            .in(Menu.selection("arranger_clip"), Menu.selection("arranger_audio_note"),
                Menu.selection("arranger_clip_audio_note"))
            .menuOnly()
            .visibleWhen(context -> {
                Object item = selectedItem(context);
                return item instanceof DyF || item instanceof poA;
            })
            .onRun(BwAraActions::edit);
    }

    private static Object selectedItem(ActionContext context) {
        if (!(context.selection() instanceof bju selection))
            return null;
        var target = selection.unz().ogS();
        List<?> items = target == null ? null : target.bK_(); // 认右键打开菜单时的对象，不追着后来的选区跑。
        if (items == null || items.size() != 1)
            return null;
        Object selected = items.getFirst();
        return selected instanceof eOj clip ? clip.qVz() : selected;
    }

    private static poA onlyRegion(DyF event) throws IOException {
        try (Stream<?> regions = event.vws()) {
            var selected = regions.limit(2).toList();
            if (selected.size() == 1 && selected.getFirst() instanceof poA region)
                return region;
        }
        throw new IOException("For clips with several audio events, select one event in the audio editor.");
    }

    private static Selection capture(Object item) throws IOException {
        poA region = item instanceof DyF clip ? onlyRegion(clip)
            : item instanceof poA selected ? selected : null;
        if (region == null)
            throw new IOException("Select a single Arranger audio clip.");
        DyF event = owningClip(region);
        if ((item instanceof DyF clip && event != clip) || !(event.unz(CmH.class) instanceof CmH project))
            throw new IOException("Select a single Arranger audio clip.");
        fRJ asset = region.GKF();
        if (asset == null || asset.tWW() == null)
            throw new IOException("The region has no available audio file.");
        Path source = asset.tWW().toPath().toRealPath();
        if (!source.getFileName().toString().toLowerCase(Locale.ROOT).endsWith(".wav"))
            throw new IOException("This version accepts local WAV sources.");
        if (project.PJ3() == null)
            throw new IOException("This Bitwig project has no media directory yet.");
        return new Selection(project, event, region, asset, source);
    }

    private static DyF owningClip(poA region) throws IOException {
        // 共用一个 WAV 很正常；共用事件本身才会让一次改音传到别的片段。
        var owners = clipReferences(region);
        if (owners.size() > 1)
            throw new IOException("This audio event is shared by several clips. Edit an independent clip instead.");
        if (owners.size() == 1 && owners.getFirst() instanceof DyF clip)
            return clip;
        throw new IOException("Select an audio event in a unique Arranger clip.");
    }

    private static List<?> clipReferences(poA region) {
        var content = region.xDy();
        if (content == null)
            return List.of();
        try (Stream<?> references = content.zzi()) {
            return references.limit(2).toList();
        }
    }

    private static void edit(ActionContext context) {
        if (!EDITING.compareAndSet(false, true)) {
            Notifications.toast("Finish the open Melodyne edit first.");
            return;
        }
        try {
            Selection selection = capture(selectedItem(context));
            Path takes = Files.createDirectories(MOD.resource("takes")).toRealPath();
            Path host = MOD.resource("host").toAbsolutePath();
            Path engine = MOD.resource("engine-loader").toAbsolutePath();
            String common = System.getenv("CommonProgramFiles");
            Path plugin = Path.of(common == null ? "C:/Program Files/Common Files" : common,
                "VST3", "Celemony", "Melodyne", "Melodyne.vst3");
            if (!Files.exists(plugin))
                throw new IOException("Melodyne VST3 was not found in Common Files.");
            Thread.ofVirtual().name("BwARA editor").start(() -> run(selection, takes, host, engine, plugin));
        } catch (Exception error) {
            EDITING.set(false);
            report(error);
        }
    }

    private static void run(Selection selection, Path takes, Path host, Path engine, Path plugin) {
        Path take = null;
        Process bridge = null;
        LiveSelection live = null;
        boolean preserveLog = true;
        try {
            Path previous = selection.source().getParent();
            boolean legacy = previous != null && takes.equals(previous.getParent())
                && selection.source().getFileName().toString().equals(EDITED);
            take = Files.createTempDirectory(takes, "session-");
            Files.writeString(take.resolve("owner"), Long.toString(PID), StandardCharsets.US_ASCII);
            try {
                reapExpiredSessions(takes);
            } catch (IOException ignored) {
                // 缓存目录暂时不可遍历时，仍然允许编辑。
            }
            Path stagedInput = take.resolve(SOURCE);
            Path rendered = take.resolve(EDITED);
            Path archive = take.resolve(ARCHIVE);
            Path priorArchive = take.resolve("previous.ara");
            // 首版把源音频和存档放在 takes 里。留这条小路，让老工程也能回家。
            if (legacy) {
                Files.copy(previous.resolve(SOURCE), stagedInput);
                Files.copy(previous.resolve(ARCHIVE), priorArchive);
            } else if (!TakeAudio.extract(selection.source(), stagedInput, priorArchive)) {
                Files.copy(selection.source(), stagedInput);
            }
            bridge = new ProcessBuilder(engine.toString(), Long.toString(ProcessHandle.current().pid()), take.toString())
                .directory(engine.getParent().toFile()).redirectErrorStream(true)
                .redirectOutput(take.resolve("engine.log").toFile()).start();
            awaitEngine(bridge, take);
            List<String> command = new ArrayList<>(List.of(host.toString(), "live", plugin.toString(),
                stagedInput.toString(), rendered.toString(), archive.toString()));
            command.add(Files.exists(priorArchive) ? priorArchive.toString() : "-");
            command.add("--follow");
            command.add(takes.resolve("editor-size.txt").toString());
            command.add(Long.toString(PID));
            Path directory = take;
            var session = onUi(() -> {
                // 等素材准备好再确认目标还在。检查、启动窗口和登记会话放在同一个 UI 回合。
                if (gEH.xw3().Anf(selection.project()) == null || !selection.matches(selection.asset()))
                    return null;
                if (owningClip(selection.region()) != selection.event())
                    return null;
                Process process = new ProcessBuilder(command).directory(directory.toFile())
                    .redirectError(directory.resolve(LOG).toFile()).start();
                active = new LiveSelection(selection, directory, stagedInput, process);
                return active;
            }).get();
            if (session == null) {
                preserveLog = false;
                return;
            }
            live = session;
            Process process = session.editor;
            Thread transport = Thread.ofVirtual().name("BwARA transport").start(() -> follow(session, process));
            Thread commands = Thread.ofVirtual().name("BwARA editor commands").start(session::receiveTransport);
            try {
                session.run();
                if (session.end == null && process.exitValue() != 0)
                    throw new IOException("Melodyne did not finish. See " + take.resolve(LOG));
                preserveLog = false;
            } finally {
                transport.interrupt();
                if (process.isAlive())
                    process.destroyForcibly().waitFor();
                commands.interrupt();
            }
        } catch (Exception error) {
            Exception cause = error instanceof ExecutionException && error.getCause() instanceof Exception inner
                ? inner : error;
            var session = live;
            drf.EO().asyncExec(() -> {
                if (session != null)
                    session.failCheckpoints(cause);
                report(cause);
            });
        } finally {
            stopEngine(bridge, take);
            EDITING.set(false);
            if (take != null) {
                try {
                    if (!preserveLog) {
                        removeSession(take);
                    }
                } catch (IOException error) {
                    MOD.log("Could not clean up temporary edit files", error);
                }
            }
        }
    }

    private static void removeSession(Path directory) throws IOException {
        try (var files = Files.newDirectoryStream(directory)) {
            for (Path file : files) {
                if (Files.isRegularFile(file, java.nio.file.LinkOption.NOFOLLOW_LINKS))
                    Files.deleteIfExists(file);
            }
        }
        Files.deleteIfExists(directory);
    }

    private static void reapExpiredSessions(Path takes) throws IOException {
        long expiry = System.currentTimeMillis() - TimeUnit.DAYS.toMillis(7);
        try (var sessions = Files.newDirectoryStream(takes, "session-*")) {
            for (Path directory : sessions) {
                if (!Files.isDirectory(directory, java.nio.file.LinkOption.NOFOLLOW_LINKS))
                    continue;
                Path owner = directory.resolve("owner");
                try {
                    // 只认本版登记过的缓存。工程素材和早期 takes 可能仍有引用。
                    if (Files.isRegularFile(owner, java.nio.file.LinkOption.NOFOLLOW_LINKS)
                            && Files.getLastModifiedTime(directory).toMillis() < expiry
                            && ProcessHandle.of(readNumber(owner)).filter(ProcessHandle::isAlive).isEmpty())
                        removeSession(directory);
                } catch (IOException | IllegalArgumentException ignored) {
                    // 缓存回收失败不应阻止下一次编辑。
                }
            }
        }
    }

    private static long readNumber(Path path) throws IOException {
        return Long.parseLong(Files.readString(path, StandardCharsets.US_ASCII).strip());
    }

    private static void awaitEngine(Process bridge, Path take) throws IOException, InterruptedException {
        for (int attempt = 0; attempt < 100 && bridge.isAlive(); attempt++) {
            if (Files.isRegularFile(take.resolve("engine-ready")))
                return;
            Thread.sleep(50);
        }
        throw new IOException("Activate the audio engine. The BwARA adapter could not attach. See " + take.resolve("engine.log"));
    }

    private static void stopEngine(Process bridge, Path take) {
        if (bridge == null)
            return;
        try {
            Files.createFile(take.resolve("engine-stop"));
            if (!bridge.waitFor(2, TimeUnit.SECONDS))
                bridge.destroyForcibly().waitFor();
        } catch (IOException | InterruptedException error) {
            bridge.destroyForcibly();
            MOD.log("Could not stop the BwARA engine session", error);
        }
    }

    static boolean deferSave(CmH project, Runnable resume, Pv callback) {
        return deferCheckpoint(project, new CheckpointRequest(resume, "BwARA project save", callback));
    }

    static boolean deferAudioExport(CmH project, Runnable resume) {
        return deferCheckpoint(project, new CheckpointRequest(resume, "BwARA audio export", null));
    }

    private static boolean deferCheckpoint(CmH project, CheckpointRequest request) {
        var session = active;
        if (session == null || session.selection.project() != project || session.end == End.PROJECT_CLOSED)
            return false;
        session.checkpoints.add(request);
        return true;
    }

    static void prepareClose(CmH project) {
        var session = active;
        if (session == null || session.selection.project() != project || project.np2() || !session.hasTarget())
            return;
        var modified = project.ERa().beI();
        if (!modified.getBoolValue() && session.hasPendingEdit())
            modified.setBoolValue(true);
    }

    static void closeEditor(CmH project) {
        var session = active;
        if (session != null && session.selection.project() == project)
            session.stop(End.PROJECT_CLOSED);
    }

    private record CheckpointRequest(Runnable resume, String errorTitle, Pv callback) {
        void fail(Exception error) {
            String message = error.getMessage() == null ? error.toString() : error.getMessage();
            MOD.log(errorTitle, error);
            Notifications.post(Severity.ERROR, errorTitle, message);
            if (callback != null)
                callback.beI(message);
        }
    }

    private record ProjectMedia(Path path, long frames) {}
    private record PreviewTarget(Path media, boolean needsProxy) {}

    private enum End { TARGET_CHANGED, PROJECT_CLOSED }

    private static final class LiveSelection {
        // 一次会话只认一个工程、一个事件。工程对象只在 UI 线程碰，文件工作交给后台。
        private final Selection selection;
        private final Path directory;
        private final Path original;
        private final Process editor;
        private final LinkedBlockingQueue<CheckpointRequest> checkpoints = new LinkedBlockingQueue<>();
        // 下面三项只在 Bitwig UI 线程读写。
        private fRJ current;
        private long closeCheckToken;
        private long storedRevision;
        private volatile End end;
        // 后台线程拥有这些版本号；0 表示还没有，不是“第零次改音”。
        private long revision;      // 已通知引擎的版本
        private long proxyRevision; // 当前私有 WAV 对应的基准版本，供引擎核对 PCM
        private long proxyFrames;
        private int nextProxyPadding = ThreadLocalRandom.current().nextInt(1024, 65536);
        private long saveToken;
        private long engineRevision;
        private long engineBasis;
        private long consumedRevision;

        private LiveSelection(Selection selection, Path directory, Path original, Process editor) {
            this.selection = selection;
            this.directory = directory;
            this.original = original;
            this.editor = editor;
            current = selection.asset();
        }

        private void run() throws Exception {
            while (end == null && editor.isAlive()) {
                if (Files.deleteIfExists(directory.resolve("save-shortcut"))) {
                    onUi(() -> {
                        if (end != End.PROJECT_CLOSED)
                            ProjectSave.fromEditor(selection.project());
                        return null;
                    }).get();
                }
                CheckpointRequest request = checkpoints.poll(30, TimeUnit.MILLISECONDS);
                if (request == null) {
                    long latest = latestPreview();
                    publish(latest);
                    if (latest <= revision)
                        consumePreview(latest);
                    continue;
                }
                long saved;
                try {
                    saved = flush();
                    checkpoint(saved, request.resume());
                } catch (Exception error) {
                    onUi(() -> {
                        if (hasTarget())
                            request.fail(error);
                        else if (end == End.TARGET_CHANGED)
                            request.resume().run();
                        return null;
                    }).get();
                    continue;
                }
                consumePreview(saved);
            }
            if (end != null) {
                if (!editor.waitFor(2, TimeUnit.SECONDS))
                    editor.destroyForcibly().waitFor();
            }
            if (end == End.PROJECT_CLOSED)
                return;

            checkpoint(end == null ? latestPreview() : 0, () -> {
                active = null;
                for (CheckpointRequest request; (request = checkpoints.poll()) != null;)
                    request.resume().run();
            });
        }

        private boolean hasPendingEdit() {
            // 问的是“改没改”，不是“渲染完没完”。刚拖完音符就关工程，也得算数。
            Path query = directory.resolve("close-check-" + (closeCheckToken + 1));
            try {
                if (editor.isAlive()) {
                    Files.createFile(query.resolveSibling(query.getFileName() + ".request"));
                    ++closeCheckToken;
                    Path response = query.resolveSibling(query.getFileName() + ".ready");
                    long end = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(150);
                    while (editor.isAlive() && System.nanoTime() < end) {
                        if (Files.isRegularFile(response))
                            return readRevision(response) > storedRevision;
                        Thread.sleep(4);
                    }
                }
                if (!editor.isAlive())
                    return latestPreview() > storedRevision;
            } catch (IOException | NumberFormatException error) {
                MOD.log("Could not check the latest Melodyne edit before closing", error);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
            }
            // 插件忙得没空回话时，交给 Bitwig 的保存提示决定；沉默不等于同意丢稿。
            return true;
        }

        private boolean hasTarget() {
            // 播放跟随会定期来问，停着时删事件或换素材也能收掉窗口。
            if (end == null && !selection.matches(current))
                stop(End.TARGET_CHANGED);
            return end == null;
        }

        private boolean hasWritableTarget() {
            if (!hasTarget())
                return false;
            // 查引用会遍历编曲区，只在发布音频或保存时做，播放光标不用跟着查。
            var owners = clipReferences(selection.region());
            if (owners.size() == 1 && owners.getFirst() == selection.event())
                return true;
            stop(End.TARGET_CHANGED);
            Notifications.toast("Melodyne closed because the audio event no longer has a unique clip.");
            return false;
        }

        private void stop(End reason) {
            boolean alreadyStopping = end != null;
            end = reason;
            // 编辑对象变了，已经排队的保存和导出还要继续。工程真关了才清队列。
            if (reason == End.PROJECT_CLOSED) {
                active = null;
                checkpoints.clear();
            }
            if (alreadyStopping)
                return;
            try {
                // 沿用宿主进程认识的信号，让它跳过最后一次渲染。
                Files.createFile(directory.resolve("project-closed"));
            } catch (IOException error) {
                MOD.log("Could not signal Melodyne to close", error);
                editor.destroy();
            }
        }

        private void failCheckpoints(Exception error) {
            if (active == this)
                active = null;
            for (CheckpointRequest request; (request = checkpoints.poll()) != null;)
                request.fail(error);
        }

        private long flush() throws Exception {
            long token = ++saveToken;
            Files.createFile(directory.resolve("save-" + token + ".request"));
            Path response = directory.resolve("save-" + token + ".ready");
            long start = System.nanoTime();
            while (end == null && editor.isAlive() && Files.notExists(response)) {
                if (System.nanoTime() - start >= TimeUnit.SECONDS.toNanos(60))
                    throw new IOException("Melodyne did not finish preparing the edit. Try again.");
                Thread.sleep(20);
            }
            if (end != null)
                return 0;
            if (Files.isRegularFile(response))
                return readRevision(response);
            if (editor.exitValue() == 0)
                return latestPreview();
            throw new IOException("Melodyne stopped before preparing the edit. See " + directory.resolve(LOG));
        }

        private static long readRevision(Path response) throws IOException {
            return readNumber(response);
        }

        private Path snapshot(long version, String suffix) {
            return directory.resolve("preview-" + version + suffix);
        }

        private long latestPreview() throws IOException {
            try {
                return readRevision(directory.resolve("preview.ready"));
            } catch (NoSuchFileException notReady) {
                return 0; // 还没有完成的预览；刚打开窗口不算改了音。
            }
        }

        private void publish(long newest) throws Exception {
            if (newest <= revision)
                return;

            PreviewTarget target = onUi(() -> {
                if (!hasWritableTarget() || selection.project().IDq())
                    return null;
                return new PreviewTarget(selection.mediaDirectory(), proxyRevision == 0 || proxyIsShared());
            }).get();
            if (target == null)
                return;
            // 复制可能带走旧代理。每份新代理都使用不同的长度，旧副本会回到文件里那一版。
            var proxy = target.needsProxy() ? writeProjectMedia(target.media(), newest, nextProxyPadding++) : null;
            long frames = proxy == null ? proxyFrames : proxy.frames();
            long basis = proxy == null ? proxyRevision : newest;
            Path next = directory.resolve("engine-preview.next");
            Files.writeString(next, newest + " " + frames + " " + basis + "\n", StandardCharsets.US_ASCII);
            Path command = directory.resolve("engine-preview.txt");
            // 音频写盘期间可能出现新的引用。核对、挂素材和通知引擎放在同一个 UI 回合。
            if (!onUi(() -> {
                if (!hasWritableTarget() || selection.project().IDq())
                    return false;
                // 刚才准备命令时若又多了一个引用，下轮先做新代理再发这版音频。
                if (proxy == null && proxyIsShared())
                    return false;
                if (proxy != null)
                    attach(proxy);
                try {
                    Files.move(next, command, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                } catch (AtomicMoveNotSupportedException error) {
                    Files.move(next, command, StandardCopyOption.REPLACE_EXISTING);
                }
                return true;
            }).get()) {
                if (proxy != null)
                    Files.deleteIfExists(proxy.path());
                return;
            }
            proxyFrames = frames;
            proxyRevision = basis;
            revision = newest;
            engineRevision = newest;
            engineBasis = proxyRevision;
        }

        private boolean proxyIsShared() {
            var references = current.BZt; // Bitwig 会登记和解除素材的 glu 引用。
            return references != null && references.size() > 1;
        }

        private void consumePreview(long version) throws IOException {
            if (version <= consumedRevision || end != null)
                return;
            // 保留引擎命令引用的两份文件，方便它在重启后重新接入。
            // 保存与发布共用此工作线程，不会在复制过程中删掉同一份快照。
            long newest = latestPreview();
            try (var files = Files.newDirectoryStream(directory, "preview-*")) {
                for (Path file : files) {
                    String name = file.getFileName().toString();
                    if (!name.endsWith(".wav") && !name.endsWith(".ara"))
                        continue;
                    long candidate = Long.parseLong(name.substring(8, name.length() - 4));
                    if (candidate < newest && candidate != engineRevision && candidate != engineBasis) {
                        try {
                            Files.deleteIfExists(file);
                        } catch (IOException ignored) {
                            // 解码器还开着旧文件时，下次发布再回收。
                        }
                    }
                }
            }
            Files.writeString(directory.resolve("preview-consumed"), Long.toString(version), StandardCharsets.US_ASCII);
            consumedRevision = version;
        }

        private void receiveTransport() {
            try (var input = new BufferedReader(new InputStreamReader(editor.getInputStream(), StandardCharsets.US_ASCII))) {
                for (String line; end == null && (line = input.readLine()) != null;) {
                    String[] fields = line.split(" ", 2);
                    if (fields.length != 2 || !List.of("start", "stop", "seek", "toggle").contains(fields[0]))
                        continue;
                    double seconds = Double.parseDouble(fields[1]);
                    onUi(() -> {
                        if (active == this && hasTarget())
                            selection.transportCommand(fields[0], seconds);
                        return null;
                    }).get();
                }
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            } catch (IOException | ExecutionException | NumberFormatException error) {
                if (editor.isAlive())
                    MOD.log("Could not receive Melodyne transport requests", error);
            }
        }

        private void checkpoint(long version, Runnable after) throws Exception {
            Path media = onUi(() -> version > storedRevision && hasWritableTarget()
                ? selection.mediaDirectory() : null).get();
            var saved = media == null ? null : writeProjectMedia(media, version, 0);
            long start = System.nanoTime();
            // 换入完整音频，再启动 Bitwig 的保存或导出，必须同一个 UI 回合完成。
            // 后台在这里等着，免得下一次预览抢先把引用换走。
            while (!onUi(() -> {
                if (end == End.PROJECT_CLOSED)
                    return true;
                if (selection.project().IDq())
                    return false;
                if (saved != null && hasWritableTarget()) {
                    attach(saved);
                    storedRevision = version;
                }
                after.run();
                return true;
            }).get()) {
                if (System.nanoTime() - start >= TimeUnit.SECONDS.toNanos(60))
                    throw new IOException("Bitwig is still saving this project. Try again.");
                Thread.sleep(30);
            }
            if (saved != null && end == null) {
                // 已保存的 WAV 就此封存。继续改音时另建代理，磁盘上的存档不追着编辑跑。
                proxyRevision = 0;
                revision = version;
            }
        }

        private ProjectMedia writeProjectMedia(Path media, long version, int padding) throws IOException {
            Files.createDirectories(media);
            Path temporary = Files.createTempFile(media, ".bwara-", ".partial");
            Path published = media.resolve((padding != 0 ? "BwARA-live-" : "BwARA-") + UUID.randomUUID() + ".wav");
            try {
                Files.copy(snapshot(version, ".wav"), temporary, StandardCopyOption.REPLACE_EXISTING);
                long frames = TakeAudio.padPreview(temporary, padding);
                TakeAudio.embed(temporary, original, snapshot(version, ".ara"));
                Files.move(temporary, published);
                return new ProjectMedia(published, frames);
            } finally {
                Files.deleteIfExists(temporary);
            }
        }

        private void attach(ProjectMedia media) throws IOException {
            if (!selection.mediaDirectory().equals(media.path().getParent()))
                throw new IOException("The project media folder moved. Try saving again.");
            // 我们自己发布的素材接着认，外面换进来的素材才结束旧会话。
            current = selection.apply(media.path(), media.frames());
        }

        private TransportSnapshot transport() {
            if (!hasTarget())
                return new TransportSnapshot(0, 0, null);
            var sample = selection.transport();
            return new TransportSnapshot(sample.mode(), sample.seconds(), DockPanel.bounds(selection.project()));
        }
    }

    private static <T> CompletableFuture<T> onUi(java.util.concurrent.Callable<T> action) {
        // 文件线程可以等 UI，UI 不反过来等整个渲染；否则“保存”就成了“卡住”。
        var request = new CompletableFuture<T>();
        drf.EO().asyncExec(() -> {
            if (!request.isCancelled()) {
                try {
                    request.complete(action.call());
                } catch (Exception error) {
                    request.completeExceptionally(error);
                }
            }
        });
        return request;
    }

    private static void follow(LiveSelection selection, Process process) {
        try (var output = new OutputStreamWriter(process.getOutputStream(), StandardCharsets.US_ASCII)) {
            TransportSnapshot previous = null;
            while (process.isAlive()) {
                var request = onUi(selection::transport);
                try {
                    TransportSnapshot sample = request.get(200, TimeUnit.MILLISECONDS);
                    // 播放时持续喂位置；停下后只发变化，别让同一个坐标刷屏。
                    if (sample.mode() == 1 || !sample.equals(previous)) {
                        var dock = sample.dock();
                        output.write(sample.mode() + " " + sample.seconds() + (dock == null ? ""
                            : " " + dock.x() + " " + dock.y()
                                + " " + dock.width() + " " + dock.height()) + "\n");
                        output.flush();
                        previous = sample;
                    }
                } catch (TimeoutException error) {
                    request.cancel(false);
                }
                Thread.sleep(24);
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        } catch (IOException | ExecutionException error) {
            if (process.isAlive())
                MOD.log("The Melodyne editor stopped following the Bitwig transport", error);
        }
    }

    private record TransportSnapshot(int mode, double seconds, DockPanel.Bounds dock) {}

    private static void report(Exception error) {
        MOD.log("Could not apply Melodyne edit", error);
        Notifications.post(Severity.ERROR, "BwARA", error.getMessage() == null ? error.toString() : error.getMessage());
    }

    private record Selection(CmH project, DyF event, poA region, fRJ asset, Path source) {
        TransportSnapshot transport() {
            var transport = project.jPe();
            double beat = transport.bcT().getDoubleValue();
            double localBeat = er.beI(beat, event);
            var time = event.dH_();
            // 拉伸、偏移、循环都让 Bitwig 自己算；我们只把源文件秒数告诉 Melodyne。
            double seconds = region.beI(localBeat - region.ie(), time.lZ_(), time.sCA(region.ie()));
            boolean inRegion = beat >= event.ie() && beat < event.oiY()
                && localBeat >= region.ie() && localBeat < region.oiY();
            int mode = transport.QDQ() ? (inRegion ? 1 : 2) : 0;
            return new TransportSnapshot(mode, Math.max(0, Math.min(asset.Bqk(), seconds)), null);
        }

        void transportCommand(String command, double seconds) {
            var transport = project.jPe();
            if (command.equals("toggle")) {
                transport.neH(!transport.QDQ());
            } else if (command.equals("start")) {
                transport.neH(true);
            } else if (command.equals("stop")) {
                transport.neH(false);
            } else if (Double.isFinite(seconds)) {
                var time = event.dH_();
                double local = region.ie() + region.Gef(seconds, time.lZ_(), time.sCA(region.ie()));
                local = Math.max(region.ie(), Math.min(Math.nextDown(region.oiY()), local));
                double beat = er.beI(local, event, 0);
                var loop = event.qaW();
                if (loop.beI() && loop.ckt() > 0 && local >= loop.ie()) {
                    // 同一源位置可能在工程里出现多次，取离当前播放头最近的一次。
                    double period = loop.ckt();
                    double first = Math.max(0, Math.ceil((event.ie() - beat) / period));
                    double last = Math.floor((Math.nextDown(event.oiY()) - beat) / period);
                    double nearest = Math.rint((transport.bcT().getDoubleValue() - beat) / period);
                    if (first <= last)
                        beat += period * Math.max(first, Math.min(last, nearest));
                }
                if (Double.isFinite(beat)) {
                    beat = Math.max(event.ie(), Math.min(Math.nextDown(event.oiY()), beat));
                    // 最后一个参数让停着的引擎也移动播放头，下一次 Play 才会从这里起跑。
                    transport.beI(beat, false, true);
                    MOD.log("Melodyne seek to source " + seconds + " s, project beat " + beat);
                }
            }
        }

        Path mediaDirectory() throws IOException {
            if (project.PJ3() == null)
                throw new IOException("The project media directory is unavailable.");
            return project.PJ3().Gef().toPath();
        }

        private boolean matches(fRJ current) {
            // 坐标可以变，会话认的是原片段里的事件和它当前应有的素材。
            if (event.unz(CmH.class) != project || region.GKF() != current)
                return false;
            try (Stream<?> regions = event.vws()) {
                return regions.anyMatch(candidate -> candidate == region);
            }
        }

        fRJ apply(Path rendered, long frames) throws IOException {
            // 用 Bitwig 自己的导入器认文件，撤销栈里只换这个事件的素材引用。
            fRJ replacement = fRJ.beI(new Lpf(rendered.toFile()));
            final double expectedDuration = (double) frames / asset.byf();
            if (Math.abs(replacement.Bqk() - expectedDuration) > 0.5 / asset.byf() || replacement.byf() != asset.byf()
                    || replacement.ymo() != asset.ymo())
                throw new IOException("The rendered file does not match the original sample format or duration.");
            dEy edit = new dEy();
            if (!edit.beI(region, "Apply Melodyne Edit"))
                throw new IOException("Bitwig refused the edit transaction.");
            try {
                region.p1J().sCA(glu.Gef(replacement));
            } finally {
                edit.Gef(); // 事务开了就得关，抛异常也不能留下半扇门。
            }
            fRJ applied = region.GKF(); // Bitwig 可能给同一文件登记另一个对象，取回它实际留下的那个。
            if (applied == null || applied.tWW() == null || !Files.isSameFile(applied.tWW().toPath(), rendered))
                throw new IOException("Bitwig did not retain the rendered sample reference.");
            MOD.log("Applied ARA take to existing audio region: " + rendered);
            return applied;
        }
    }
}

