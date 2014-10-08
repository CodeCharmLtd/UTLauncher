#include "utlauncher.h"
#include "serverbrowser.h"
#include "awesome.h"

#include <QStatusBar>

#include "configdialog.h"
#include <QProgressDialog>

/*
#undef signals                                                  
extern "C" {                                                                 
  #include <libappindicator/app-indicator.h>
  #include <gtk/gtk.h>

  void quitIndicator(GtkMenu *, gpointer);

}                                                                            
#define signals public

void quitIndicator(GtkMenu *menu, gpointer data) {
  Q_UNUSED(menu);
  QApplication *self = static_cast<QApplication *>(data);

  self->quit();
}

void ShowUnityAppIndicator()
{
    AppIndicator *indicator;
    GtkWidget *menu, *item;

    menu = gtk_menu_new();

    item = gtk_menu_item_new_with_label("Quit");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    g_signal_connect(item, "activate",
                 G_CALLBACK(quitIndicator), qApp);  // We cannot connect
                 // gtk signal and qt slot so we need to create proxy
                 // function later on, we pass qApp pointer as an argument.
                 // This is useful when we need to call signals on "this"
                 //object so external function can access current object
    gtk_widget_show(item);

    indicator = app_indicator_new(
    "unique-application-name",
        "indicator-messages",
      APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );

    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator, GTK_MENU(menu));
}*/

QtAwesome* awesome;

QColor UTLauncher::iconColor() const {
    if(getenv("ICON_COLOR")) {
        return QColor(getenv("ICON_COLOR"));
    } else {
        QWidget w;
        return w.palette().color(w.foregroundRole());
    }
}

UTLauncher::UTLauncher(int& argc, char** argv) :
            QApplication(argc, argv),
            settings(QSettings::IniFormat, QSettings::UserScope, "CodeCharm", "UTLauncher"),
            bootstrap(settings), systemTray(QIcon(":/icon.png")) {
    QPixmap pixmap(":/splash.jpg");
    splash = new UTSplash(pixmap);
    
    awesome = new QtAwesome(qApp);
    awesome->initFontAwesome();
    
    awesome->setDefaultOption( "color" , iconColor() );
    awesome->setDefaultOption( "color-active" , iconColor() );
    awesome->setDefaultOption( "color-selected" , iconColor() );
    
    splash->setGeometry(
        QStyle::alignedRect(
            Qt::LeftToRight,
            Qt::AlignCenter,
            splash->size(),
            qApp->desktop()->availableGeometry()
        ));
    
    settings.setValue("FirstRun", false);
    
    settings.sync();
    
    bootstrap.start();
    connect(&bootstrap, &Bootstrap::message, [=](QString msg) {
        splash->showMessage(msg);
    });
    
    connect(&bootstrap, &Bootstrap::ready, this, &UTLauncher::prepareConfig);
    
    browser = new ServerBrowser();
    connect(&bootstrap, &Bootstrap::serversInfo, this, &UTLauncher::gotServersInfo, Qt::QueuedConnection );
    
    splash->showMessage("Getting version information...");
    
    if(!settings.value("StartMinimized", false).toBool()) {
        splash->show();
    }
}

void UTLauncher::gotServersInfo(QJsonDocument document)
{
    browser->loadFromJson(document.object());
    serversRefreshTimer.singleShot(5*60000, &bootstrap, SLOT(refreshServers()));
}

void UTLauncher::prepareConfig()
{
    startServerBrowser();
}

void UTLauncher::closeSplash()
{
    if(splash) {
        splash->deleteLater();
        splash = nullptr;
    }
}

#include <3rdparty/quazip/quazip/quazip.h>
#include <3rdparty/quazip/quazip/quazipfile.h>
#include <QMessageBox>

void UTLauncher::startServerBrowser()
{
    splashTimer.singleShot(2000, this, SLOT(closeSplash()));

    if(!settings.value("StartMinimized", false).toBool()) {
        browser->show();
    } else {
        qApp->setQuitOnLastWindowClosed(false); // workaround for app not starting
    }
    browser->setMOTD(bootstrap.MOTD());
    
    auto hasEditorSupport = [=] {
        QString editorPath = bootstrap.editorExePath();
        QString projectPath = bootstrap.projectPath();
        return QFile::exists(editorPath) && QFile::exists(projectPath);
    };
    
    auto openSettings = [=](bool mandatoryEditor = false) {
        ConfigDialog dialog(settings, mandatoryEditor);
        dialog.exec();
        browser->setEditorSupport(hasEditorSupport());
        
        browser->setHideOnClose(settings.value("MinimizeToTrayOnClose").toBool());
    };
    
    connect(browser, &ServerBrowser::openServer, [=](QString url, bool spectate, bool inEditor) {

        if(inEditor) {
            QString editorPath = bootstrap.editorExePath();
            QString projectPath = bootstrap.projectPath();
            if(!editorPath.length() || !projectPath.length()) {
                openSettings(true);
                return;
            }
            QProcess::startDetached(editorPath, QStringList() << projectPath << "-GAME" << (url + (spectate?"?SpectatorOnly=1":"")) );
        } else {
            QString exePath = bootstrap.programExePath();
            if(!exePath.length()) {
                openSettings();
                return;
            }
            const auto serverEntry = browser->serverEntryFromAddress(url);
            auto launch = [=] {
                qDebug() << "Launching!!\n";
              QProcess::startDetached(exePath, QStringList()
#ifdef LAUNCH_WITH_UE4
                << "UnrealTournament"
#endif
                << (url + (spectate?"?SpectatorOnly=1":"")) 
                << "-SaveToUserDir");
            };
                        
            if(serverEntry) {
                if(bootstrap.isStockMap(serverEntry->map)) {
                    launch();
                    return;
                }
                

                QString exePath = bootstrap.programExePath();
                QFileInfo exeInfo(exePath);
                auto contentDir = QDir(exeInfo.dir());
                contentDir.cdUp();
                contentDir.cdUp();
                contentDir.cd("Content");
                auto zipFilePath = contentDir.absoluteFilePath(serverEntry->map + ".zip");
                
                if(QFile::exists(zipFilePath)) {
                    launch();
                    return;
                }

                Download mapDownload;
                mapDownload.setTarget("https://ut.rushbase.net/customcontent/Data/" + serverEntry->map + ".zip");
                QProgressDialog dialog("Downloading map: " + serverEntry->map, "Cancel", 0, 100);
                
                QFile zipFile(zipFilePath);
                zipFile.open(QIODevice::WriteOnly);
                
                int httpCode = 200;
                connect(&mapDownload, &Download::error, [&](int code) {
                    httpCode = code;
                    if(code != 200) {
                        zipFile.remove();
                        QMessageBox::critical(nullptr, "Unable to download map", QString("Got code %1 while trying to download map:<br>%2").arg(code).arg(serverEntry->map));
                    }
                });
                
                connect(&mapDownload, &Download::chunk, [&](QByteArray chunk) {
                    qDebug() << "Reading!\n";
                    zipFile.write(chunk);
                });
                connect(&mapDownload, &Download::progress, [&](double progress) {
                    dialog.setValue(100*progress);
                    if(progress == 1.0) {
                        dialog.accept();
                    }
                });
                
                
                mapDownload.download();
                qDebug() << "Downloading map " << serverEntry->map;
                if(!dialog.exec() || httpCode != 200) {
                    zipFile.remove(); // remove unfinished download
                    return;
                }
                zipFile.close();
                
                QProgressDialog installDialog("Installing map: " + serverEntry->map, "", 0, 100);
                installDialog.setWindowModality(Qt::ApplicationModal);
                installDialog.show();
                installDialog.setValue(0);
                
                qDebug() << "Installed!!\n";
                QuaZip zip(zipFilePath);
                
                auto textCodec = QTextCodec::codecForName("TSCII");
                zip.setFileNameCodec(textCodec);
                zip.setCommentCodec(textCodec);
                zip.open(QuaZip::mdUnzip);

                QuaZipFile file(&zip);

                size_t totalSize = 0;
                for(bool f=zip.goToFirstFile(); f; f=zip.goToNextFile()) {
                    QuaZipFileInfo info;
                    zip.getCurrentFileInfo(&info);
                    totalSize += info.uncompressedSize;
                }
                qDebug() << "Total size " << totalSize;
                size_t accumulatedSize = 0;
                /* TODO!!!!: this whole thing needs to be redone asynchronously */
                for(bool f=zip.goToFirstFile(); f; f=zip.goToNextFile()) {
                    file.open(QIODevice::ReadOnly);
                    //same functionality as QIODevice::readData() -- data is a char*, maxSize is qint64
                    //file.readData(data,maxSize);
                    QString filename = file.getActualFileName();
                    if(file.isOpen()) {
                        qApp->processEvents();
                        qDebug() << installDialog.isVisible() << installDialog.value();
                        if(!installDialog.isVisible() && installDialog.value() != 100 && installDialog.value() != -1)
                            return;
                        bool isDir = (filename.right(1) == "/");
                        if(isDir) {
                            contentDir.mkpath(filename);
                        } else {
                            QFile f(contentDir.absoluteFilePath(filename));
                            f.open(QIODevice::WriteOnly);
                            auto data = file.readAll();
                            accumulatedSize += data.size();
                            qDebug() << accumulatedSize;
                            installDialog.setValue(100 * (double)accumulatedSize / totalSize);
                            qApp->processEvents();
                            qDebug() << installDialog.isVisible() << installDialog.value();
                            if(!installDialog.isVisible() && installDialog.value() != 100 && installDialog.value() != -1)
                                return;

                            f.write(data);
                        }
                        qDebug() << file.getActualFileName() << isDir;
                        
                        //do something with the data
                        if(file.isOpen())
                            file.close();
                        qApp->processEvents();
                        qDebug() << installDialog.isVisible() << installDialog.value();
                        if(!installDialog.isVisible() && installDialog.value() != 100 && installDialog.value() != -1)
                            return;
                    }
                    else {
                        qDebug() << "Cannot open" << filename << "inside zip";;
                    }
                }
                qDebug() << "Closing zip\n";
                zip.close();

                launch();
            }            
        }
    });
    
    disconnect(&bootstrap, &Bootstrap::ready, this, &UTLauncher::prepareConfig);
    
    connect(&bootstrap, &Bootstrap::ready, this, [=] {
        browser->setMOTD(bootstrap.MOTD());
    });
    
    connect(browser, &ServerBrowser::openSettings, openSettings);
    
    browser->setEditorSupport(hasEditorSupport());
    
    browser->setHideOnClose(settings.value("MinimizeToTrayOnClose", false).toBool());
    
    systemTray.setToolTip("UTLauncher");
    auto systemTrayMenu = new QMenu(browser);
    
    auto showBrowser = new QAction(awesome->icon(fa::listalt), "Server List", this);
    connect(showBrowser, &QAction::triggered, [=]() {
        browser->showNormal();
        browser->raise();
        browser->activateWindow();
    });
    
    auto runUTAction = new QAction(awesome->icon( fa::gamepad ),"Run UT", this);
    connect(runUTAction, &QAction::triggered, [=]() {
        QString exePath = bootstrap.programExePath();
        if(!exePath.length()) {
            browser->show();
            openSettings();
            return;
        }
        QProcess::startDetached(exePath
#ifdef LAUNCH_WITH_UE4
        , QStringList() << "UnrealTournament" << "-SaveToUserDir"
#endif
        );
    });

    auto runEditorAction = new QAction(awesome->icon( fa::code ),"Run Editor", this);
    connect(runEditorAction, &QAction::triggered, [=]() {
        QString editorPath = bootstrap.editorExePath();
        QString projectPath = bootstrap.projectPath();
        QProcess::startDetached(editorPath, QStringList() << projectPath);
    });
    

    auto quitAction = new QAction(awesome->icon( fa::signout ),"Quit", this);
    connect(quitAction, &QAction::triggered, [=]() {
        QApplication::quit();
    });
    
    systemTrayMenu->addAction(showBrowser);
    systemTrayMenu->addSeparator();
    systemTrayMenu->addAction(runUTAction);
    systemTrayMenu->addAction(runEditorAction);
    systemTrayMenu->addSeparator();
    systemTrayMenu->addAction(quitAction);
    
    systemTray.setContextMenu(systemTrayMenu);
    
    //desktop type
    QString desktop;
    bool is_unity;

    desktop = getenv("XDG_CURRENT_DESKTOP");
    is_unity = (desktop.toLower() == "unity");
    
    if(is_unity)
    {
        // shows unity appindicator
        // ShowUnityAppIndicator(); //TODO: implement appindicator
        // hide qt systemtray - not working on unity
        systemTray.hide();
    }
    else
    {
        systemTray.show();

        connect(&systemTray, &QSystemTrayIcon::activated, [=](QSystemTrayIcon::ActivationReason reason)
        {
            qApp->setQuitOnLastWindowClosed(true);
            runEditorAction->setVisible(hasEditorSupport());
            switch(reason)
            {
            case QSystemTrayIcon::Trigger:
            {
                if(browser->isHidden())
                {
                    browser->show();
                }
                else
                {
                    browser->hide();
                }
                break;
                }
            }
        });
    }
}
