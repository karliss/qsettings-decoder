#include <iostream>
#include <QCommandLineParser>
#include <QFile>
#include <QDebug>
#include <QSettings>
#include <QFileInfo>
#include <QStack>

enum class Action {
    GetValue,
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

    parser.process(QCoreApplication::arguments());

    auto setAction = [&](Action action) {
        if (result.action != Action::NotSet) {
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
        setAction(Action::GetValue);
        result.query = parser.value(getValueOption);
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
        case Action::NotSet:
        case Action::Error: {
            parser.showHelp(1);
        }
    }

    return 0;
}