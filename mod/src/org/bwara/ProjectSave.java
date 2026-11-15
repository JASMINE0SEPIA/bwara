package org.bwara;

import bitwig.obf.aCG;
import bitwig.obf.aYN;
import bml.api.Arg;
import bml.api.At;
import bml.api.Hook;
import com.bitwig.base.async.Pv;
import com.bitwig.flt.app.gEH;
import com.bitwig.flt.document.core.master.CmH;

public final class ProjectSave {
    // 此处只接管保存的时机；路径、另存为、完成回调仍由 Bitwig 决定。
    private static final String SAVE = "beI(LaCG;Ljava/io/File;ZZLcom/bitwig/base/async/Pv;)V";
    private static final ThreadLocal<Context> context = new ThreadLocal<>();

    private record Context(CmH project, Pv callback) {}

    private ProjectSave() {}

    @Hook(target = gEH.class, method = SAVE, at = At.ENTRY)
    public static void captureSave(@Arg(0) aCG document, @Arg(4) Pv callback) {
        if (document != null && document.Anf() instanceof CmH project)
            context.set(new Context(project, callback));
        else
            context.remove();
    }

    // Runnable 已经带着本次保存的路径和回调，先等 Melodyne 交齐作业，再原样放行。
    @Hook(target = gEH.class, method = SAVE, at = At.REPLACE_CALL,
          callOwner = aYN.class, call = "beI(Ljava/lang/Runnable;)V")
    public static void dispatchSave(Runnable save) {
        Context target = context.get();
        context.remove();
        Runnable resume = () -> aYN.beI(save);
        if (target == null || !BwAraActions.deferSave(target.project(), resume, target.callback()))
            resume.run();
    }

    @Hook(target = gEH.class, method = "Gef(LaCG;ZLdwF;Lcom/bitwig/base/async/Pv;)V", at = At.ENTRY)
    public static void prepareClose(@Arg(0) aCG document, @Arg(1) boolean confirm) {
        if (confirm && !document.BCC() && document.Anf() instanceof CmH project)
            BwAraActions.prepareClose(project);
    }

    @Hook(target = gEH.class, method = "beI(LdwF;LaCG;Ljava/lang/Runnable;)V", at = At.ENTRY)
    public static void prepareGroupClose(@Arg(1) aCG document) {
        prepareClose(document, true);
    }

    @Hook(target = gEH.class, method = "sCA(LaCG;)V", at = At.ENTRY)
    public static void closingProject(@Arg(0) aCG document) {
        // “想关工程”和“真的关了”是两回事；窗口寿命跟着后者。
        if (document.Anf() instanceof CmH project)
            BwAraActions.closeEditor(project);
    }

    static void fromEditor(CmH project) {
        var app = gEH.xw3();
        var document = app.Anf(project);
        if (document != null)
            app.beI(document, false, false, null, null);
    }
}
