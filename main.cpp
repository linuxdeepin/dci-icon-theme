// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <QGuiApplication>
#include <QImageReader>
#include <QCommandLineParser>
#include <QDirIterator>
#include <QBuffer>
#include <QDebug>

#include <DDciFile>

DCORE_USE_NAMESPACE

static inline void dciChecker(bool result) {
    if (!result) {
        qWarning() << "Failed on writing dci file";
        exit(-6);
    }
}

static inline QByteArray webpImageData(const QImage &image, int quality) {
    QByteArray data;
    QBuffer buffer(&data);
    bool ok = buffer.open(QIODevice::WriteOnly);
    Q_ASSERT(ok);
    dciChecker(image.save(&buffer, "webp", quality));
    return data;
}

static bool writeScaledImage(DDciFile &dci, const QString &imageFile, const QString &targetDir, int scale/* = 2*/)
{
    int size = scale * 256;
    QImageReader image(imageFile);
    if (!image.canRead()) {
        qWarning() << "Ignore the null image file:" << imageFile;
        return false;
    }

    if (image.supportsOption(QImageIOHandler::ScaledSize)) {
        image.setScaledSize(QSize(size, size));
    }

    dciChecker(dci.mkdir(targetDir + QString("/%1").arg(scale)));
    const QImage &img = image.read().scaledToWidth(size, Qt::SmoothTransformation);
    const QByteArray &data = webpImageData(img, 100);
    dciChecker(dci.writeFile(targetDir + QString("/%1/1.webp").arg(scale), data));

    return true;
}

static bool writeImage(DDciFile &dci, const QString &imageFile, const QString &targetDir)
{
    return writeScaledImage(dci, imageFile, targetDir, 2) &&
            writeScaledImage(dci, imageFile, targetDir, 3);
}

static bool recursionLink(DDciFile &dci, const QString &fromDir, const QString &targetDir)
{
    for (const auto &i : dci.list(fromDir, true)) {
        const QString file(fromDir + "/" + i);
        const QString targetFile(targetDir + "/" + i);
        if (dci.type(file) == DDciFile::Directory) {
            if (!dci.mkdir(targetFile))
                return false;
            if (!recursionLink(dci, file, targetFile))
                return false;
        } else {
            if (!dci.link(file, targetFile))
                return false;
        }
    }

    return true;
}

static QByteArray readNextSection(QIODevice *io) {
    QByteArray section;
    char ch;

    bool oneLine = true;
    while (io->getChar(&ch)) {
        if (!oneLine && ch == '"') { // ["] end
            if (io->peek(1) == ",")
                io->skip(1); // skip [,]
            break;
        }  else if (ch == ',') {
            break;
        } else if (ch == '"') { // ["] begin
            oneLine = false;
            continue;
        }

        section.append(ch);

        if(oneLine && io->peek(1) == "\n")
            break;
    }

    return section.trimmed();
}

QMultiHash<QString, QString> parseIconFileSymlinkMap(const QString &csvFile) {
    QFile file(csvFile);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed on open symlink map file:" << csvFile;
        exit(-7);
    }

    QMultiHash<QString, QString> map;
    while (!file.atEnd()) {
        QByteArray key = readNextSection(&file);
        QByteArray value = readNextSection(&file);
        for (const auto &i : value.split('\n')) {
            map.insert(QString::fromLocal8Bit(key), QString::fromLocal8Bit(i));
        }

        char ch = 0;
        while (file.getChar(&ch) && ch != '\n');
    }

    qInfo() << "Got symlinks:" << map.size();

    return map;
}

void makeLink(const QFileInfo &file, const QDir &outputDir, const QString &dciFilePath,
              const QMultiHash<QString, QString> &symlinksMap)
{
    if (symlinksMap.contains(file.completeBaseName())) {
        const QString symlinkKey = QFileInfo(dciFilePath).fileName();
        for (const auto &symTarget : symlinksMap.values(file.completeBaseName())) {
            const QString newSymlink = outputDir.absoluteFilePath(symTarget + ".dci");
            qInfo() << "Create symlink from" << symlinkKey << "to" << newSymlink;
            if (!QFile::link(symlinkKey, newSymlink)) {
                qWarning() << "Failed on create symlink from" << symlinkKey << "to" << newSymlink;
            }
        }
    }
}

void doFixDarkTheme(const QFileInfo &file, const QDir &outputDir, const QMultiHash<QString, QString> &symlinksMap)
{
    const QString &newFile = outputDir.absoluteFilePath(file.fileName());

    DDciFile dciFile(file.absoluteFilePath());
    if (!dciFile.isValid()) {
        qWarning() << "Skip invalid dci file:" << file.absoluteFilePath();
        return;
    }

    for (const auto &i : dciFile.list("/")) {
        if (dciFile.type(i) != DDciFile::Directory)
            continue;

        for (const auto &j : dciFile.list(i)) {
            if (dciFile.type(j) != DDciFile::Directory || !j.endsWith(".light"))
                continue;

            const QString darkDir(j.left(j.size() - 5) + "dark");
            Q_ASSERT(darkDir.endsWith(".dark"));
            if (!dciFile.exists(darkDir)) {
                dciChecker(dciFile.mkdir(darkDir));
                dciChecker(recursionLink(dciFile, j, darkDir));
            }
        }
    }

    dciChecker(dciFile.writeToFile(newFile));

    makeLink(file, outputDir, newFile, symlinksMap);
}

int main(int argc, char *argv[])
{
    QCommandLineOption fileFilter({"m", "match"}, "Give wildcard rules on search icon files, "
                                                  "Each eligible icon will be packaged to a dci file, "
                                                  "If the icon have the dark mode, it needs to store "
                                                  "the dark icon file at \"dark/\" directory relative "
                                                  "to current icon file, and the file name should be "
                                                  "consistent.", "wildcard palette");
    QCommandLineOption outputDirectory({"o", "output"}, "Save the *.dci files to the given directory.",
                                       "directory");
    QCommandLineOption symlinkMap({"s", "symlink"}, "Give a csv file to create symlinks for the output icon file."
                                                    "\nThe content of symlink.csv like:\n"
                                                    "\t\t sublime-text, com.sublimetext.2\n"
                                                    "\t\t deb, \"\n"
                                                    "\t\t application-vnd.debian.binary-package\n"
                                                    "\t\t application-x-deb\n"
                                                    "\t\t gnome-mime-application-x-deb\n"
                                                    "\t\t \"\n"
                                  ,
                                       "csv file");
    QCommandLineOption fixDarkTheme("fix-dark-theme", "Create symlinks from light theme for dark theme files.");

    QGuiApplication a(argc, argv);
    a.setApplicationName("dci-icon-theme");
    a.setApplicationVersion("0.0.2");

    QCommandLineParser cp;
    cp.setApplicationDescription("dci-icon-theme tool is a command tool that generate dci icons from common icons.\n"
                                 "For example, the tool is used in the following ways: \n"
                                 "\t dci-icon-theme /usr/share/icons/hicolor/256x256/apps ~/Desktop/hicolor\n"
                                 "\t dci-icon-theme -m *.png /usr/share/icons/hicolor/256x256/apps ~/Desktop/hicolor\n"
                                 "\t dci-icon-theme --fix-dark-theme <input dci files directory> -o  <output directory path> \n"
                                 "\t dci-icon-theme <input file directory> -o  <output directory path> -s ~/Desktop/symlink.csv \n"""
                                 );

    cp.addOptions({fileFilter, outputDirectory, symlinkMap, fixDarkTheme});
    cp.addPositionalArgument("source", "Search the given directory and it's subdirectories, "
                                       "get the files conform to rules of --match.",
                             "~/dci-png-icons");
    cp.addHelpOption();
    cp.addVersionOption();
    cp.process(a);

    if (a.arguments().size() == 1)
        cp.showHelp(-1);

    if (cp.positionalArguments().isEmpty()) {
        qWarning() << "Not give a source directory.";
        cp.showHelp(-2);
    }

    if (!cp.isSet(outputDirectory)) {
        qWarning() << "Not give -o argument";
        cp.showHelp(-4);
    }

    QDir outputDir(cp.value(outputDirectory));
    if (!outputDir.exists()) {
        if (!QDir::current().mkpath(outputDir.absolutePath())) {
            qWarning() << "Can't create the" << outputDir.absolutePath() << "directory";
            cp.showHelp(-5);
        }
    } else {
        qErrnoWarning("The output directory have been exists.");
        return -1;
    }

    QMultiHash<QString, QString> symlinksMap;
    if (cp.isSet(symlinkMap)) {
        symlinksMap = parseIconFileSymlinkMap(cp.value(symlinkMap));
    }

    const QStringList nameFilter = cp.isSet(fileFilter) ? cp.values(fileFilter) : QStringList();
    const auto sourceDirectory = cp.positionalArguments();
    for (const auto &sd : qAsConst(sourceDirectory)) {
        QDir sourceDir(sd);
        if (!sourceDir.exists()) {
            qWarning() << "Ignore the non-exists directory:" << sourceDir;
            continue;
        }

        // read all links first
        {
            QDirIterator di(sourceDir.absolutePath(), nameFilter,
                            QDir::NoDotAndDotDot | QDir::Files,
                            QDirIterator::Subdirectories);
            while (di.hasNext()) {
                di.next();
                QFileInfo file = di.fileInfo();
                 if (!file.isSymLink())
                     continue;

                 const QString &linkTarget = QFileInfo(file.readLink()).completeBaseName();
                 if (!symlinksMap.values(linkTarget).contains(file.completeBaseName())) {
                     symlinksMap.insert(linkTarget, file.completeBaseName());
                     qInfo() << "add link" << file.completeBaseName() << "->" << linkTarget;
                 } else {
//                        qDebug() << "** link already existed in symlinksMap **";
                 }
            }
        }

        QDirIterator di(sourceDir.absolutePath(), nameFilter,
                        QDir::NoDotAndDotDot | QDir::Files,
                        QDirIterator::Subdirectories);
        while (di.hasNext()) {
            di.next();
            QFileInfo file = di.fileInfo();

            if (file.isSymLink())
                continue;

            if (cp.isSet(fixDarkTheme)) {
                doFixDarkTheme(file, outputDir, symlinksMap);
                continue;
            }

            if (file.path().endsWith(QStringLiteral("/dark"))) {
                qInfo() << "Ignore the dark icon file:"  << file;
                continue;
            }

            const QString dciFilePath(outputDir.absoluteFilePath(file.completeBaseName()) + ".dci");
            if (QFile::exists(dciFilePath)) {
                qWarning() << "Skip exists dci file:" << dciFilePath;
                continue;
            }
            DDciFile dciFile;

            qInfo() << "Wrting to dci file:" << dciFilePath;

            dciChecker(dciFile.mkdir("/256"));
            dciChecker(dciFile.mkdir("/256/normal.light"));
            if (!writeImage(dciFile, file.filePath(), "/256/normal.light"))
                continue;

            dciChecker(dciFile.mkdir("/256/normal.dark"));
            QFileInfo darkIcon(file.dir().absoluteFilePath("dark/" + file.fileName()));
            if (darkIcon.exists()) {
                writeImage(dciFile, darkIcon.filePath(), "/256/normal.dark");
            } else {
                dciChecker(recursionLink(dciFile, "/256/normal.light", "/256/normal.dark"));
            }

            dciChecker(dciFile.writeToFile(dciFilePath));

            makeLink(file, outputDir, dciFilePath, symlinksMap);
        }
    }

    return 0;
}
