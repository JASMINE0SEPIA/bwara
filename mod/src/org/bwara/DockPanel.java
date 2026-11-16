package org.bwara;

import bitwig.obf.aCZ;
import bitwig.obf.akJ;
import bitwig.obf.bac;
import bitwig.obf.dwF;
import bml.panels.PanelProvider;
import bml.panels.Panels;
import bml.panels.ViewContext;
import com.bitwig.base.geom.uXa;
import com.bitwig.flt.document.core.master.CmH;

import java.util.ArrayList;

public final class DockPanel implements PanelProvider {
    // 每个布局可能建立自己的面板；只用当前工程和所属窗口里正显示的那个。
    private static final ArrayList<Surface> panels = new ArrayList<>();

    @Override
    public void declare(Panels views) {
        views.view("melodyne", "Melodyne").icon("panel-icon").scale(false).content(DockPanel::create);
    }

    private static Surface create(ViewContext context) {
        return new Surface(context);
    }

    static Bounds bounds(CmH project) {
        Bounds visible = null;
        for (var panel : panels) {
            if (!panel.TUb() || !panel.pWO() || panel.rtK() <= 0 || panel.cBV() <= 0 || !panel.context.shown()
                    || !(panel.context.window() instanceof aCZ window) || window.cdX() != project)
                continue;
            long x = 0;
            long y = 0;
            boolean selected = false;
            for (dwF view = panel; view != null; view = view.oRH()) {
                if (view instanceof bac widget) {
                    // 标签页可能还挂在布局里。读 Bitwig 当前选中的页，别让旧坐标挡着隔壁面板。
                    var node = widget.yAy();
                    if (node != null && node.BBg(akJ.class) instanceof akJ tabs) {
                        var choice = tabs.beI(window.yX3());
                        selected = choice != null && node.Fhl().equals(choice.getValue());
                    }
                }
                var position = view.sk();
                x += position.MgQ();
                y += position.Anf();
            }
            if (!selected)
                continue;
            if (visible != null)
                return null;
            visible = new Bounds(x, y, panel.rtK(), panel.cBV());
        }
        return visible;
    }

    record Bounds(long x, long y, long width, long height) {}

    private static final class Surface extends dwF {
        private final ViewContext context;

        Surface(ViewContext context) {
            this.context = context;
            I_();
            panels.add(this);
        }

        @Override public dwF neH() { return this; }
        @Override public uXa mM_() { return uXa.beI(); }
        @Override public uXa nk_() { return uXa.Gef(); }

        @Override
        public void iz_() {
            panels.remove(this);
            super.iz_();
        }
    }
}
