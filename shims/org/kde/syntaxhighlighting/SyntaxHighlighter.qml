// Windows shim for org.kde.syntaxhighlighting's SyntaxHighlighter.
// Real backend: see Repository.qml -- whatever ends up providing syntax
// definitions on Windows. Completely inert for now: holds the properties ii
// assigns (textEdit/repository/definition/theme) but never actually
// highlights anything.
import QtQml

QtObject {
    property var textEdit: null
    property var repository: null
    property var definition: null
    property var theme: null
}
