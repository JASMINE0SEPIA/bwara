package org.bwara;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** WAV 正常播放，原始音频和 ARA 存档藏在私有块里；搬工程时不用找散落一地的配件。 */
final class TakeAudio {
    private static final long MAX_RIFF_SIZE = 0xffff_ffffL;
    private static final int RIFF = 0x52494646;
    private static final int WAVE = 0x57415645;
    private static final int SOURCE = 0x62776153; // bwaS
    private static final int ARCHIVE = 0x62776141; // bwaA

    private TakeAudio() {}

    /** 在源音频末尾补静音，给引擎一个辨认私有素材的长度标签，不改变事件的播放范围。 */
    static long padPreview(Path preview, int padding) throws IOException {
        try (var wave = new RandomAccessFile(preview.toFile(), "rw")) {
            long end = riffEnd(wave);
            if (end != wave.length() || wave.readInt() != 0x666d7420 || readSize(wave) != 16)
                throw new IOException("Unexpected Melodyne preview WAV header.");
            int format = Short.toUnsignedInt(Short.reverseBytes(wave.readShort()));
            int channels = Short.toUnsignedInt(Short.reverseBytes(wave.readShort()));
            wave.seek(36);
            if (format != 3 || (channels != 1 && channels != 2) ||
                    wave.readInt() != 0x66616374 || readSize(wave) != 4)
                throw new IOException("Unexpected Melodyne preview sample format.");
            long frames = readSize(wave);
            if (wave.readInt() != 0x64617461)
                throw new IOException("Missing Melodyne preview sample data.");
            long bytes = readSize(wave);
            long paddedBytes = bytes + (long) padding * channels * Float.BYTES;
            long paddedEnd = end + paddedBytes - bytes;
            if (bytes != frames * channels * Float.BYTES || end != 56 + bytes || paddedEnd - 8 > MAX_RIFF_SIZE)
                throw new IOException("The Melodyne preview cannot be extended.");
            wave.seek(end);
            wave.write(new byte[Math.toIntExact(paddedEnd - end)]);
            wave.seek(4);
            wave.writeInt(Integer.reverseBytes((int) (paddedEnd - 8)));
            wave.seek(44);
            wave.writeInt(Integer.reverseBytes((int) (frames + padding)));
            wave.seek(52);
            wave.writeInt(Integer.reverseBytes((int) paddedBytes));
            return frames + padding;
        }
    }

    static void embed(Path rendered, Path source, Path archive) throws IOException {
        // 每个已发布的 WAV 都是独立快照。原始音频只嵌一层，重开时先 extract 再编辑。
        try (var wave = new RandomAccessFile(rendered.toFile(), "rw")) {
            long end = riffEnd(wave);
            long sourceSize = Files.size(source);
            long archiveSize = Files.size(archive);
            long length = end + 16 + padded(sourceSize) + padded(archiveSize);
            if (end != wave.length() || sourceSize == 0 || archiveSize == 0 || length - 8 > MAX_RIFF_SIZE)
                throw new IOException("The edited audio and ARA state exceed the RIFF/WAVE size limit.");
            wave.seek(end);
            append(wave, SOURCE, source, sourceSize);
            append(wave, ARCHIVE, archive, archiveSize);
            wave.seek(4);
            wave.writeInt(Integer.reverseBytes((int) (length - 8)));
        }
    }

    static boolean extract(Path rendered, Path source, Path archive) throws IOException {
        try (var wave = new RandomAccessFile(rendered.toFile(), "r")) {
            var state = readState(wave);
            if (state == null)
                return false;
            state.original().write(wave, source);
            state.archive().write(wave, archive);
            return true;
        }
    }

    static boolean restoreArchive(Path rendered, Path source, Path archive) throws IOException {
        try (var wave = new RandomAccessFile(rendered.toFile(), "r")) {
            var state = readState(wave);
            // 同名或同长度的 WAV 也可能是另一遍录音。逐块比对源文件，免得拿错音来套旧存档。
            if (state == null || !state.original().matches(wave, source))
                return false;
            state.archive().write(wave, archive);
            return true;
        }
    }

    private static State readState(RandomAccessFile wave) throws IOException {
        long end = riffEnd(wave);
        Chunk original = null;
        Chunk archive = null;
        while (wave.getFilePointer() < end) {
            if (end - wave.getFilePointer() < 8)
                throw new IOException("Truncated WAV chunk header.");
            int id = wave.readInt();
            long size = readSize(wave);
            long start = wave.getFilePointer();
            if (start + padded(size) > end)
                throw new IOException("Truncated WAV chunk.");
            if (id == SOURCE && original == null)
                original = new Chunk(start, size);
            else if (id == ARCHIVE && archive == null)
                archive = new Chunk(start, size);
            else if (id == SOURCE || id == ARCHIVE)
                throw new IOException("Duplicate BwARA state in WAV.");
            wave.seek(start + padded(size));
        }
        if (original == null && archive == null)
            return null;
        if (original == null || archive == null || original.size == 0 || archive.size == 0)
            throw new IOException("This WAV contains incomplete BwARA state.");
        return new State(original, archive);
    }

    private static long riffEnd(RandomAccessFile wave) throws IOException {
        if (wave.readInt() != RIFF)
            throw new IOException("BwARA requires a RIFF/WAVE source.");
        long end = 8 + readSize(wave);
        if (wave.readInt() != WAVE || end < 12 || end > wave.length())
            throw new IOException("Invalid RIFF/WAVE header.");
        return end;
    }

    private static long readSize(RandomAccessFile wave) throws IOException {
        return Integer.toUnsignedLong(Integer.reverseBytes(wave.readInt()));
    }

    private static long padded(long size) {
        // RIFF 的块按偶数字节对齐；那个填充字节不算在块自己的长度里。
        return size + (size & 1);
    }

    private static void append(RandomAccessFile wave, int id, Path file, long length) throws IOException {
        wave.writeInt(id);
        wave.writeInt(Integer.reverseBytes((int) length));
        try (var input = Files.newInputStream(file)) {
            byte[] bytes = new byte[64 * 1024];
            int count;
            while ((count = input.read(bytes)) != -1)
                wave.write(bytes, 0, count);
        }
        if ((length & 1) != 0)
            wave.write(0);
    }

    private record State(Chunk original, Chunk archive) {}

    private record Chunk(long start, long size) {
        boolean matches(RandomAccessFile wave, Path path) throws IOException {
            if (Files.size(path) != size)
                return false;
            wave.seek(start);
            try (var input = Files.newInputStream(path)) {
                byte[] expected = new byte[64 * 1024];
                byte[] actual = new byte[expected.length];
                for (long remaining = size; remaining > 0;) {
                    int count = (int) Math.min(remaining, expected.length);
                    wave.readFully(expected, 0, count);
                    if (input.readNBytes(actual, 0, count) != count ||
                            !Arrays.equals(expected, 0, count, actual, 0, count))
                        return false;
                    remaining -= count;
                }
                return input.read() == -1;
            }
        }

        void write(RandomAccessFile wave, Path path) throws IOException {
            wave.seek(start);
            try (var output = Files.newOutputStream(path)) {
                byte[] bytes = new byte[64 * 1024];
                for (long remaining = size; remaining > 0;) {
                    int count = (int) Math.min(remaining, bytes.length);
                    wave.readFully(bytes, 0, count);
                    output.write(bytes, 0, count);
                    remaining -= count;
                }
            }
        }
    }
}
