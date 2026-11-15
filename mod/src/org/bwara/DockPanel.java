package org.bwara;

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
            if (!panel.TUb() || panel.rtK() <= 0 || panel.cBV() <= 0 || !panel.context.shown()
                    || !(panel.context.window() instanceof bitwig.obf.aCZ window) || window.cdX() != project)
                continue;
            if (visible != null)
                return null;
            long x = 0;
            long y = 0;
            for (dwF view = panel; view != null; view = view.oRH()) {
                var position = view.sk();
                x += position.MgQ();
                y += position.Anf();
            }
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
