package org.bwara;

import bitwig.obf.DyF;
import bitwig.obf.bMt;
import bitwig.obf.eIn;
import bitwig.obf.nEr;
import bitwig.obf.oq1;
import bitwig.obf.poA;
import com.bitwig.flt.document.core.master.CmH;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.function.DoubleUnaryOperator;

record MusicTime(List<MusicTime.Point> tempo, List<MusicTime.Meter> meter) {
    private static final double MIN_QUARTER_STEP = 1e-9;
    record Point(double seconds, double quarter) {}
    record Meter(int numerator, int denominator, double quarter) {}

    static MusicTime capture(CmH project, DyF clip, poA region, double anchor, double duration) {
        var transport = project.jPe();
        var signature = transport.rtH();
        var meters = new TreeMap<Double, Meter>();
        int initial = signature.Anf();
        meters.put(0.0, new Meter(bMt.byf(initial), bMt.Heb(initial), 0.0));
        for (Object lane : transport.IpB().beI()) {
            if (!(lane instanceof eIn automation) || automation.UnB() != signature)
                continue;
            for (Object item : automation.tWW().byf()) {
                if (item instanceof nEr marker && marker.ie() >= 0) {
                    double quarter = marker.ie() + 0.0;
                    meters.put(quarter, new Meter(marker.dU_(), marker.byf(), quarter));
                }
            }
        }

        var time = clip.dH_();
        var map = time.lZ_();
        double origin = time.sCA(region.ie());
        double start = snapOrigin(anchor + region.Gef(0, map, origin));
        double end = snapOrigin(anchor + region.Gef(duration, map, origin));
        // 倒放没有递增的源时间，ARA 的时间表表达不了这种映射。
        if (!Double.isFinite(start) || !Double.isFinite(end) || end <= start)
            return null;

        // 音频仍按原始秒数交给 Melodyne。这里只把工程的拍子投影到这段音频上。
        DoubleUnaryOperator sourceAt = quarter -> region.beI(quarter - anchor, map, origin);
        var knots = new TreeSet<Double>();
        knots.add(start);
        knots.add(end);
        if (start < 0 && end > 0)
            knots.add(0.0);
        var tempoLane = transport.KrW();
        if (tempoLane != null) {
            for (Object item : tempoLane.tWW().byf()) {
                if (item instanceof oq1 marker)
                    addInside(knots, marker.ie(), start, end);
            }
        }
        var warp = region.Ml2();
        if (warp != null) {
            for (int i = 0; i < warp.Hqm(); ++i)
                addInside(knots, anchor + (warp.sCA(i).ie() - region.qJX()), start, end);
        }
        var points = new ArrayList<Point>();
        var previous = new Point(sourceAt.applyAsDouble(start), start);
        points.add(previous);
        for (double quarter : knots.tailSet(start, false)) {
            var next = new Point(sourceAt.applyAsDouble(quarter), quarter);
            // 冻结段也会让拍子挤成同一个采样点。
            if (!Double.isFinite(previous.seconds()) || !Double.isFinite(next.seconds())
                    || next.seconds() <= previous.seconds())
                return null;
            subdivide(sourceAt, previous, next, points, 0);
            if (points.size() >= 131072)
                return null;
            previous = next;
        }
        // 素材在后半首也要保留第 1 小节的位置，Melodyne 靠它算出正确的小节号。
        if (start > 0.0)
            points.addFirst(atOrigin(sourceAt, points.getFirst(), points.get(1)));
        else if (end < 0.0)
            points.add(atOrigin(sourceAt, points.getLast(), points.get(points.size() - 2)));
        return new MusicTime(List.copyOf(points), List.copyOf(meters.values()));
    }

    private static double snapOrigin(double quarter) {
        return Math.abs(quarter) < MIN_QUARTER_STEP ? 0 : quarter;
    }

    private static Point atOrigin(DoubleUnaryOperator sourceAt, Point edge, Point neighbor) {
        double seconds = sourceAt.applyAsDouble(0);
        // Bitwig 会把素材范围外的 warp 时间夹到边界，ARA 那里还得留出递增的拍子。
        if (!Double.isFinite(seconds) || (seconds - edge.seconds()) * edge.quarter() >= 0) {
            double secondsPerQuarter = (neighbor.seconds() - edge.seconds()) / (neighbor.quarter() - edge.quarter());
            seconds = edge.seconds() - edge.quarter() * secondsPerQuarter;
        }
        return new Point(seconds, 0.0);
    }

    private static void addInside(TreeSet<Double> knots, double quarter, double start, double end) {
        if (!Double.isFinite(quarter) || quarter <= start + MIN_QUARTER_STEP || quarter >= end - MIN_QUARTER_STEP)
            return;
        double left = knots.floor(quarter);
        double right = knots.ceiling(quarter);
        if (quarter - left > MIN_QUARTER_STEP && right - quarter > MIN_QUARTER_STEP)
            knots.add(quarter);
    }

    private static void subdivide(DoubleUnaryOperator sourceAt, Point left, Point right,
                                  List<Point> points, int depth) {
        // ARA 用直线连速度点。四分之一处也检查，免得 S 曲线刚好在中点骗过我们。
        double mid = (left.quarter() + right.quarter()) * 0.5;
        if (depth < 14 && mid > left.quarter() && mid < right.quarter()) {
            double middle = sourceAt.applyAsDouble(mid);
            if (middle > left.seconds() && middle < right.seconds()
                    && (offLine(sourceAt, left, right, 0.25) || offLine(sourceAt, left, right, 0.5)
                        || offLine(sourceAt, left, right, 0.75))) {
                var point = new Point(middle, mid);
                subdivide(sourceAt, left, point, points, depth + 1);
                subdivide(sourceAt, point, right, points, depth + 1);
                return;
            }
        }
        points.add(right);
    }

    private static boolean offLine(DoubleUnaryOperator sourceAt, Point left, Point right, double fraction) {
        double quarter = left.quarter() + fraction * (right.quarter() - left.quarter());
        double seconds = left.seconds() + fraction * (right.seconds() - left.seconds());
        return Math.abs(sourceAt.applyAsDouble(quarter) - seconds) > 0.0001;
    }

    static void publish(Path directory, MusicTime grid) throws IOException {
        var file = directory.resolve("music-time.txt");
        if (grid == null) {
            Files.deleteIfExists(file);
            return;
        }
        var next = directory.resolve("music-time.next");
        Files.writeString(next, grid.encode(), StandardCharsets.US_ASCII);
        // Melodyne 要么读到整张新表，要么读到整张旧表。
        Files.move(next, file, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    private String encode() {
        var result = new StringBuilder().append(tempo.size()).append(' ').append(meter.size()).append('\n');
        for (var point : tempo)
            result.append(point.seconds()).append(' ').append(point.quarter()).append('\n');
        for (var signature : meter)
            result.append(signature.numerator()).append(' ').append(signature.denominator())
                .append(' ').append(signature.quarter()).append('\n');
        return result.toString();
    }
}
