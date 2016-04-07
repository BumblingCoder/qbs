import qbs
import QbsFunctions

Product {
    Depends { name: "Qt.core" }
    property string minimumQtVersion: "5.1.0"
    cpp.defines: {
        var res = ["QT_NO_CAST_FROM_ASCII", "QT_NO_PROCESS_COMBINED_ARGUMENT_START"];
        if (qbs.toolchain.contains("msvc"))
            res.push("_SCL_SECURE_NO_WARNINGS");
        return res;
    }
    condition: QbsFunctions.versionIsAtLeast(Qt.core.version, minimumQtVersion)
}
