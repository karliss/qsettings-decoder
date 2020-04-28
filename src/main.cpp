#include <iostream>
#include <QCommandLineParser>
#include <QFile>
#include <QDebug>
#include <QSettings>
#include <QFileInfo>
#include <QStack>
#include <QDataStream>
#include <QtCore>

#include <QJsonObject>
#include <QJsonDocument>

enum class Action {
    GetValue,
    DecodeState,
    NotSet,
    Error,
};

struct CommandLineOptions {
    Action action = Action::Error;
    QString inputFile;
    QString query;
};

CommandLineOptions parseCommandLine(QCommandLineParser &parser) {
    CommandLineOptions result;
    result.action = Action::NotSet;

    parser.addPositionalArgument("source", "Input file.");

    QCommandLineOption getValueOption({"g", "get-value"}, "Get single value with specified key", "key");
    parser.addOption(getValueOption);
    QCommandLineOption decodeState({"s", "decode-state"}, "Decode binary file containing result of MainWindow::saveState" );
    parser.addOption(decodeState);

    parser.process(QCoreApplication::arguments());

    auto setAction = [&](Action action) {
        if (result.action == Action::NotSet) {
            result.action = action;
        } else {
            qCritical() << "Can't specify multiple actions.";
            result.action = Action::Error;
        }
    };

    auto args = parser.positionalArguments();
    if (args.length() != 1) {
        qCritical() << "Input not specified";
        return {};
    }
    result.inputFile = args[0];

    if (parser.isSet(getValueOption)) {
        qDebug() << "should not be set";
        setAction(Action::GetValue);
        result.query = parser.value(getValueOption);
    }
    if (parser.isSet(decodeState)) {
        setAction(Action::DecodeState);
    }

    return result;
}

static int getSingleValue(const QString &filePath, const QString &key) {
    QFileInfo info(filePath);

    auto format = QSettings::NativeFormat;
    if (info.suffix() == "ini") {
        format = QSettings::IniFormat;
    }

#ifdef Q_WS_WIN
    bool shouldBeFile = (format == QSettings::IniFormat);
#else
    bool shouldBeFile = true;
#endif

    if (shouldBeFile) {
        if (!info.exists()) {
            qCritical() << QString("Input file '%1' does not exist.").arg(filePath);
            return 1;
        }

        if (!info.isFile()) {
            qCritical() << QString("Input file '%1' does not exist.").arg(filePath);
            return 1;
        }
    }
    QSettings settings(filePath, format);

    auto keyPath = key.split("/");

    if (keyPath.isEmpty()) {
        qCritical() << "key path empty";
        return 1;
    }

    QStack<bool> isArray;
    for (int i = 0; i < keyPath.length() - 1; i++) {
        auto component = keyPath[i];
        if (component.endsWith(']')) {
            auto parts = component.split('[');
            if (parts.length() != 2) {
                qCritical() << "Bad path";
                return 1;
            } else {
                auto numberText = parts[1].left(parts[1].length() - 1);
                auto number = numberText.toInt();
                settings.beginReadArray(parts[0]);
                settings.setArrayIndex(number);
            }
            isArray.push_back(true);
        } else {
            settings.beginGroup(component);
            isArray.push_back(false);
        }
    }


    auto keyName = keyPath.back();
    if (!settings.contains(keyName)) {
        qCritical() << QString("Key '%1' not set").arg(keyName);
        return 1;
    }

    QVariant value = settings.value(keyName);

    switch (value.type()) {
        case QMetaType::QByteArray: {
            auto bytes = value.toByteArray();
            fwrite(bytes.data(), 1, bytes.length(), stdout);
            break;
        }
        default: {
            QTextStream out(stdout, QIODevice::WriteOnly);
            qInfo() << value;
            out << value.toString() << "\n";
            out.flush();
            break;
        }
    }

    while (!isArray.empty()) {
        if (isArray.pop()) {
            settings.endArray();
        } else {
            settings.endGroup();
        }
    }

    return 0;
}

static const uchar DOCK_WIDGET_STATE_MARKER = 253;
static const uchar FLOATING_DOCK_WIDGET_TAB_MARKER = 249;
static const uchar TOOL_BAR_STATE_MARKER = 254;
static const uchar TOOL_BAR_STATE_MARKER_EX = 252;
static const uchar TAB_MARKER = 250;
static const uchar SEQUENCE_MARKER = 252;
static const uchar WIDGET_MARKER = 251;

static int DOCK_FLAG_VISIBLE = 1;
static int DOCK_FLAG_FLOATING = 2;

bool streamGood(QDataStream &stream) {
    return stream.status() == QDataStream::Ok;
}

static QJsonObject parseDockState2(QDataStream &stream) {
    QJsonObject dock;

    uchar marker;
    if (!streamGood(stream >> marker)) {
        qWarning() << "bad dock item marker";
        return dock;
    }
    if (marker != TAB_MARKER && marker != SEQUENCE_MARKER) {
        qWarning() << QString("Expected TAB_MARKER or SEQUNCE_MARKER got %1").arg((int(marker)));
        return dock;
    }
    bool tabbed = marker == TAB_MARKER;
    int index = -1;
    if (tabbed) {
        stream >> index;
        dock["index"] = index;
    }

    uchar orientation;
    stream >> orientation;

    switch (orientation) {
        case 1:
            dock["orientation"] = "Horizontal";
            break;
        case 2:
            dock["orientation"] = "Vertical";
            break;
        default:
            dock["orientation"] = "Unrecognized";
            break;
    }

    int cnt;
    stream >> cnt;
    QJsonArray subitems;
    for (int i = 0; i < cnt; i++) {
        uchar nextMarker;
        stream >> nextMarker;
        if (!streamGood(stream)) {
            qWarning() << "failed to read marker dock 2";
            return dock;
        }
        if (nextMarker == WIDGET_MARKER) {
            QJsonObject widget;
            QString name;
            uchar flags;
            stream >> name >> flags;
            widget["name"] = name;
            widget["flags"] = int(flags);
            int x, y, w, h;
            stream >> x >> y >> w >> h;
            if (name.isEmpty()) {
                widget["dummy1"] = x;
                widget["dummy2"] = y;
                widget["dummy3"] = w;
                widget["dummy4"] = h;
                subitems.push_back(widget);
                continue;
            }
            if (flags & DOCK_FLAG_FLOATING) {
                widget["floating"] = true;
                widget["x"] = x;
                widget["y"] = y;
                widget["w"] = w;
                widget["h"] = h;
            } else {
                widget["pos"] = x;
                widget["size"] = y;
                widget["d1"] = w;
                widget["d2"] = h;
            }
            widget["visible"] = (flags & DOCK_FLAG_VISIBLE) > 0;
            subitems.push_back(widget);
        } else if (nextMarker == SEQUENCE_MARKER)  {
            int pos, size, dummy1, dummy2;
            stream >> pos >> size >> dummy1 >> dummy2;
            QJsonObject list;
            list["pos"] = pos;
            list["size"] = size;
            list["dummy1"] = dummy1;
            list["dummy2"] = dummy2;
            QJsonObject list2 = parseDockState2(stream);
            list["subitems"] = list2;

            subitems.push_back(list);
        }
    }
    dock["list"] = subitems;
    return dock;
}

static QString decodeCorner(int corner) {
    switch (corner) {
        case 1:
            return "Left";
        case 2:
            return "Right";
        case 4:
            return "Top";
        case 8:
            return "Bottom";
        default:
            return QString("Unknown %1").arg(corner);
    }
}

static QJsonObject parseDockState(QDataStream &stream) {
    QJsonObject result;
    int cnt;
    if (!streamGood(stream >> cnt)) {
        qWarning() << "Failed to read dock state count";
        return result;
    }
    QJsonArray docks;
    for (int i = 0; i < cnt; i++) {
        int pos;
        QSize size;
        auto buf = static_cast<QBuffer*>(stream.device());
        if (!streamGood(stream >> pos >> size)) {
            qWarning() << "Failed to read dock pos or size";
            return result;
        }

        QJsonObject dock = parseDockState2(stream);
        dock["pos"] = pos;
        dock["size"] = QString("(%1 %2)").arg(size.width()).arg(size.height());
        docks.push_back(dock);
        if (!streamGood(stream)) {
            return result;
        }
    }
    result["docks"] = docks;

    QSize size;
    stream >> size;
    result["central_w"] = size.width();
    result["central_h"] = size.height();

    if (!streamGood(stream)) {
        return result;
    }

    QJsonArray corners;
    for (int i = 0; i < 4; i++) {
        int corner;
        if (!streamGood(stream >> corner)) {
            qWarning() << "bad corner";
            return result;
        }
        corners.append(decodeCorner(corner));
    }
    result["corners"] = corners;

    return result;
}

static QRect unpackRect(uint geom0, uint geom1, bool *floating)
{
    *floating = geom0 & 1;
    if (!*floating)
        return QRect();

    geom0 >>= 1;

    int x = (int)(geom0 & 0x0000ffff) - 0x7FFF;
    int y = (int)(geom1 & 0x0000ffff) - 0x7FFF;

    geom0 >>= 16;
    geom1 >>= 16;

    int w = geom0 & 0x0000ffff;
    int h = geom1 & 0x0000ffff;

    return QRect(x, y, w, h);
}

static QJsonObject parseToolBarAreaLayout(QDataStream &stream, uchar marker) {
    QJsonObject result;
    int lines;
    stream >> lines;
    QJsonArray lineItems;
    for (int j = 0; j < lines; j++) {
        int pos;
        stream >> pos;
        if (pos < 0 || pos >= 4) {
            qWarning() << "Bad toolBarAreaLayout line pos";
            return result;
        }
        int cnt;
        stream >> cnt;
        QJsonObject lineItem;
        lineItem["pos"] = pos;

        QJsonArray nestedItems;
        for (int k = 0; k < cnt; k++) {
            QJsonObject itemItem;
            QString name;
            uchar shown;
            stream >> name >> shown;
            itemItem["name"] = name;
            itemItem["shown"] = int(shown);
            int itemPos, itemSize;
            stream >> itemPos >> itemSize;
            itemItem["pos"] = itemPos;
            itemItem["size"] = itemSize;

            int geom0, geom1;
            stream >> geom0;
            bool floating = false;
            QRect rect;
            if (marker == TOOL_BAR_STATE_MARKER_EX) {
                stream >> geom1;
                rect = unpackRect(geom0, geom1, &floating);
            }
            itemItem["floating"] = floating;
            itemItem["rect"] = QVariant(rect).toString();

            nestedItems.push_back(itemItem);
        }
        lineItem["items"] = nestedItems;

        lineItems.push_back(lineItem);
    }
    result["lines"] = lineItems;
    return result;
}

static QJsonObject parseState(QDataStream &stream) {
    QJsonObject result;
    int marker, v;
    stream >> marker >> v;
    if (stream.status() != QDataStream::Ok) {
        qWarning() << "Parsing error";
        return result;
    }
    result["marker"] = marker;
    result["v"] = v;
    if (marker != 0xff) {
        qWarning() << "Bad version marker";
        return result;
    }

    QJsonArray items;
    while (streamGood(stream) && !stream.atEnd()) {
        uchar marker;
        stream >> marker;
        switch (marker) {
            case DOCK_WIDGET_STATE_MARKER: {
                auto obj = parseDockState(stream);
                obj["type"] = "DOCK_WIDGET_STATE_MARKER";
                items.push_back(obj);
                break;
            }
            case FLOATING_DOCK_WIDGET_TAB_MARKER: {
                QRect geometry;
                stream >> geometry;
                auto obj = parseDockState2(stream);
                obj["type"] = "FLOATING_DOCK_WIDGET_TAB_MARKER";
                obj["geometry"] = QVariant(geometry).toString();
                break;
            }
            case TOOL_BAR_STATE_MARKER:
            case TOOL_BAR_STATE_MARKER_EX: {
                auto obj = parseToolBarAreaLayout(stream, marker);
                obj["type"] = "TOOL_BAR_STATE_MARKER";
                break;
            }
            default: {
                qWarning() << QString("Unrecognized state marker %1").arg(uint32_t(marker));
                result["items"] = items;
                return result;
            }
        }
    }
    result["items"] = items;

    return result;
}

static int decodeState(const QString &filePath) {
    QFile file(filePath);
    if (!file.exists()) {
        qCritical() << "Input file does not exist";
        return 1;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical() << "Failed to open";
        return 1;
    }
    auto bytes = file.readAll();
    QDataStream dataStream(bytes);
    auto result = parseState(dataStream);
    QJsonDocument doc(result);
    auto resultBytes = doc.toJson(QJsonDocument::Indented);
    fwrite(resultBytes.data(), 1, resultBytes.length(), stdout);
    return 0;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("qsettings-decoder");
    QCoreApplication::setApplicationVersion("0.1");

    QCommandLineParser parser;
    parser.setApplicationDescription("Utility for printing qsettings in human readable format");
    parser.addHelpOption();
    parser.addVersionOption();

    auto options = parseCommandLine(parser);
    switch (options.action) {
        case Action::GetValue: {
            return getSingleValue(options.inputFile, options.query);
        }
        case Action::DecodeState: {
            return decodeState(options.inputFile);
        }
        case Action::NotSet:
        case Action::Error: {
            parser.showHelp(1);
        }
    }

    return 0;
}