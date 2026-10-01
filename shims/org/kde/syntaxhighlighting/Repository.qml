pragma Singleton
// Windows shim for org.kde.syntaxhighlighting's Repository.
// Real backend: nothing KDE-specific is actually required long-term -- a
// bundled syntax-definition set (e.g. a vendored subset of KSyntaxHighlighting's
// XML definitions, or a different highlighting library entirely) could back
// this on Windows too. For now, every lookup returns a plain inert
// definition named after what was asked for, and MessageCodeBlock.qml's
// `SyntaxHighlighter` never actually colors anything.
import QtQml

QtObject {
    id: root

    property Component _definitionComponent: Component {
        QtObject {
            property string name: "Plain Text"
        }
    }

    function definitionForName(name) {
        return root._definitionComponent.createObject(root, {
            name: name && name.length > 0 ? name : "Plain Text",
        });
    }

    function definitionForFileName(fileName) {
        return root._definitionComponent.createObject(root, {name: "Plain Text"});
    }
}
