package org.bwara;

import bitwig.obf.NVD;
import bitwig.obf.XAe;
import bitwig.obf.anN; // Bitwig 分析后的波形数据
import bitwig.obf.drf;
import bitwig.obf.poA;
import com.bitwig.base.io.Lpf;
import com.bitwig.flt.document.core.iface.blr;
import com.bitwig.flt.document.core.iface.tIb;
import com.bitwig.flt.document.core.master.fRJ;

import java.nio.file.Path;

final class WaveformPreview {
    private static volatile WaveformPreview visible;

    private final poA region;
    private final tIb binding;
    private final fRJ sample;
    private final anN waveform;
    private final NVD progress = ignored -> drf.EO().asyncExec(() -> {
        if (visible == this)
            repaint();
    });

    private WaveformPreview(poA region, Path path, anN waveform) {
        this.region = region;
        binding = region.PJ3();
        this.waveform = waveform;
        var file = path.toFile();
        sample = fRJ.beI(new Lpf(file));
        sample.beI((Object) waveform);
        sample.beI(waveform.dvk().Gef(file));
    }

    static anN analyze(Path path) {
        return XAe.ymo().beI(new Lpf(path.toFile()));
    }

    static void show(poA region, Path path, anN waveform) {
        if (waveform == null)
            return;
        var next = new WaveformPreview(region, path, waveform);
        var previous = visible;
        visible = next;
        if (previous != null)
            previous.waveform.scr().removeListener(previous.progress);
        waveform.scr().addListener(next.progress);
        next.repaint();
    }

    static void clear(poA region) {
        var previous = visible;
        if (previous == null || previous.region != region)
            return;
        visible = null;
        previous.waveform.scr().removeListener(previous.progress);
        previous.repaint();
    }

    private void repaint() {
        // 走 Bitwig 收到波形分析结果时的刷新路径，素材引用和撤销栈都不用动。
        region.beI(region.GKF());
    }

    static blr forClip(tIb binding) {
        var preview = visible;
        // 认事件自己的素材引用。同一个 WAV 的其他片段照样画自己的波形。
        return preview != null && preview.binding == binding ? preview.sample : binding.MgQ();
    }
}
