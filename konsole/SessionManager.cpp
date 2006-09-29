// Qt
#include <QFileInfo>
#include <QFont>
#include <QList>
#include <QString>

// KDE
#include <klocale.h>
#include <krun.h>
#include <kshell.h>
#include <ksimpleconfig.h>
#include <kstandarddirs.h>

// Konsole
#include "session.h"
#include "SessionManager.h"


SessionInfo::SessionInfo(const QString& path)
{
    QString fileName = QFileInfo(path).fileName();

    QString fullPath = KStandardDirs::locate("appdata",fileName);
    Q_ASSERT( QFile::exists(fullPath) );
    
    _config = new KSimpleConfig( fullPath , true );
    _config->setDesktopGroup();
   
    _path = path;

}
SessionInfo::~SessionInfo()
{
    delete _config;
    _config = 0;
}

QString SessionInfo::name() const
{
    return _config->readEntry("Name");
}

QString SessionInfo::icon() const
{
    return _config->readEntry("Icon","konsole");
}


bool SessionInfo::isRootSession() const
{
    const QString& cmd = _config->readEntry("Exec");

    return ( cmd.startsWith("su") );
}

QString SessionInfo::command(bool stripRoot , bool stripArguments) const
{
    QString fullCommand = _config->readEntry("Exec"); 
    
    //if the .desktop file for this session doesn't specify a binary to run 
    //(eg. No 'Exec' entry or empty 'Exec' entry) then use the user's standard SHELL
    if ( fullCommand.isEmpty() )
        fullCommand = getenv("SHELL");
    
    if ( isRootSession() && stripRoot )
    {
        //command is of the form "su -flags 'commandname'"
        //we need to strip out and return just the command name part.
        fullCommand = fullCommand.section('\'',1,1);
    } 

    if ( fullCommand.isEmpty() )
        fullCommand = getenv("SHELL");
       
    if ( stripArguments ) 
        return fullCommand.section(QChar(' '),0,0);
    else
        return fullCommand;
}

QStringList SessionInfo::arguments() const
{
    QString commandString = command(false,false);

    //FIXME:  This wll fail where single arguments contain spaces (because slashes or quotation
    //marks are used) - eg. vi My\ File\ Name
    return commandString.split(QChar(' '));    
}

bool SessionInfo::isAvailable() const
{
    //TODO:  Is it necessary to cache the result of the search? 
    
    QString binary = KRun::binaryName( command(true) , false );
    binary = KShell::tildeExpand(binary);

    QString fullBinaryPath = KGlobal::dirs()->findExe(binary);

    if ( fullBinaryPath.isEmpty() )
        return false;        
    else
        return true;
}

QString SessionInfo::path() const
{
    return _path;
}


QString SessionInfo::newSessionText() const
{
    QString commentEntry = _config->readEntry("Comment");
    
    if ( commentEntry.isEmpty() )
        return i18n("New %1",name());
    else
        return commentEntry;
}

QString SessionInfo::terminal() const
{
    return _config->readEntry("Term","xterm");
}
QString SessionInfo::keyboardSetup() const
{
    return _config->readEntry("KeyTab",QString());
}
QString SessionInfo::colorScheme() const
{
    //TODO Pick a default color scheme
    return _config->readEntry("Schema");
}
QFont SessionInfo::defaultFont(const QFont& font) const
{
    if (_config->hasKey("defaultfont"))
        return QVariant(_config->readEntry("defaultfont")).value<QFont>();
    else
        return font;
}
QString SessionInfo::defaultWorkingDirectory() const
{
    return _config->readPathEntry("Cwd");
}

SessionManager::SessionManager()
{
    //locate default session
   KConfig* appConfig = KGlobal::config();
   appConfig->setDesktopGroup();

   QString defaultSessionFilename = appConfig->readEntry("DefaultSession","shell.desktop");

    //locate config files and extract the most important properties of them from
    //the config files.
    //
    //the sessions are only parsed completely when a session of this type 
    //is actually created
    QList<QString> files = KGlobal::dirs()->findAllResources("appdata", "*.desktop", false, true);

    QListIterator<QString> fileIter(files);
   
    while (fileIter.hasNext())
    { 

        QString configFile = fileIter.next();
        SessionInfo* newType = new SessionInfo(configFile);
        
        _types << newType; 
        
        if ( QFileInfo(configFile).fileName() == defaultSessionFilename )
            _defaultSessionType = newType;
    }

    Q_ASSERT( _types.count() > 0 );
    Q_ASSERT( _defaultSessionType != 0 );
}

SessionManager::~SessionManager()
{
    QListIterator<SessionInfo*> infoIter(_types);
    
    while (infoIter.hasNext())
        delete infoIter.next();
}

TESession* SessionManager::createSession(QString configPath , const QString& initialDir)
{
    TESession* session = 0;

    //select default session type if not specified
    if ( configPath.isEmpty() )
        configPath = _defaultSessionType->path();

    //search for SessionInfo object built from this config path
    QListIterator<SessionInfo*> iter(_types);
    
    while (iter.hasNext())
    {
        const SessionInfo* const info = iter.next();

        if ( info->path() == configPath )
        {
            //configuration information found, create a new session based on this
            session = new TESession();

            QListIterator<QString> iter(info->arguments());
            while (iter.hasNext())
                kDebug() << "running " << info->command(false) << ": argument " << iter.next() << endl;
            
            session->setProgram( info->command(false) );
            session->setArguments( info->arguments() );
            
            //use initial directory 
            if ( initialDir.isEmpty() )
                session->setWorkingDirectory( info->defaultWorkingDirectory() );
            else
                session->setWorkingDirectory( initialDir );

            session->setTitle( info->name() );
            session->setIconName( info->icon() );
            
            //ask for notification when session dies
            connect( session , SIGNAL(done(TESession*)) , SLOT(sessionTerminated(TESession*)) ); 

            //add session to active list            
            _sessions << session;    

            break;      
        }
    }

    Q_ASSERT( session );
    
    return session;
}

void SessionManager::sessionTerminated(TESession* session)
{
    kDebug() << __FILE__ << ": session finished " << endl;

    _sessions.remove(session);
}

QList<SessionInfo*> SessionManager::availableSessionTypes()
{
    return _types;   
}

SessionInfo* SessionManager::defaultSessionType()
{
    return _defaultSessionType;
}

#include <SessionManager.moc>