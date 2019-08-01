#include "merginapi.h"

#include <QtNetwork>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDate>
#include <QByteArray>
#include <QSet>
#include <QUuid>
#include <QtMath>

#include "inpututils.h"

const QString MerginApi::sMetadataFile = QStringLiteral( "/.mergin/mergin.json" );
const QSet<QString> MerginApi::sIgnoreExtensions = QSet<QString>() << "gpkg-shm" << "gpkg-wal" << "qgs~" << "qgz~" << "pyc" << "swap";
const QSet<QString> MerginApi::sIgnoreFiles = QSet<QString>() << "mergin.json" << ".DS_Store";


MerginApi::MerginApi( LocalProjectsManager &localProjects, QObject *parent )
  : QObject( parent )
  , mLocalProjects( localProjects )
  , mDataDir( localProjects.dataDir() )
{
  QObject::connect( this, &MerginApi::authChanged, this, &MerginApi::saveAuthData );
  QObject::connect( this, &MerginApi::apiRootChanged, this, &MerginApi::pingMergin );
  QObject::connect( this, &MerginApi::pingMerginFinished, this, &MerginApi::checkMerginVersion );

  loadAuthData();
}

void MerginApi::listProjects( const QString &searchExpression, const QString &user,
                              const QString &flag, const QString &filterTag )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return;
  }

  QNetworkRequest request;
  // projects filtered by tag "input_use"
  QString urlString = mApiRoot + QStringLiteral( "/v1/project" );
  if ( !filterTag.isEmpty() )
  {
    urlString += QStringLiteral( "?tags=" ) + filterTag;
  }
  if ( !searchExpression.isEmpty() )
  {
    urlString += QStringLiteral( "&q=" ) + searchExpression;
  }
  if ( !flag.isEmpty() )
  {
    urlString += QStringLiteral( "&flag=%1&user=%2" ).arg( flag ).arg( user );
  }
  QUrl url( urlString );
  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );

  QNetworkReply *reply = mManager.get( request );
  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
  connect( reply, &QNetworkReply::finished, this, &MerginApi::listProjectsReplyFinished );
}

void MerginApi::downloadFile( const QString &projectFullName, const QString &filename, const QString &version, int chunkNo )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return;
  }

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  QNetworkRequest request;
  QUrl url( mApiRoot + QStringLiteral( "/v1/project/raw/%1?file=%2&version=%3" ).arg( projectFullName ).arg( filename ).arg( version ) );
  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( AttrChunkNo ), QVariant( chunkNo ) );

  QString range;
  int from = UPLOAD_CHUNK_SIZE * chunkNo;
  int to = UPLOAD_CHUNK_SIZE * ( chunkNo + 1 ) - 1;
  range = QStringLiteral( "bytes=%1-%2" ).arg( from ).arg( to );
  request.setRawHeader( "Range", range.toUtf8() );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ), projectFullName );

  Q_ASSERT( !transaction.replyDownloadFile );
  transaction.replyDownloadFile = mManager.get( request );
  connect( transaction.replyDownloadFile, &QNetworkReply::finished, this, &MerginApi::downloadFileReplyFinished );

  InputUtils::log( url.toString() + " Range: " + range, QStringLiteral( "STARTED" ) );
}

void MerginApi::uploadFile( const QString &projectFullName, const QString &transactionUUID, MerginFile file, int chunkNo )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return;
  }

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  QString chunkID = file.chunks.at( chunkNo );

  QFile f( transaction.projectDir + "/" + file.path );
  QByteArray data;

  if ( f.open( QIODevice::ReadOnly ) )
  {
    f.seek( chunkNo * UPLOAD_CHUNK_SIZE );
    data = f.read( UPLOAD_CHUNK_SIZE );
  }

  QNetworkRequest request;
  QUrl url( mApiRoot + QStringLiteral( "/v1/project/push/chunk/%1/%2" ).arg( transactionUUID ).arg( chunkID ) );
  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );
  request.setRawHeader( "Content-Type", "application/octet-stream" );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ), projectFullName );

  Q_ASSERT( !transaction.replyUploadFile );
  transaction.replyUploadFile = mManager.post( request, data );
  connect( transaction.replyUploadFile, &QNetworkReply::finished, this, &MerginApi::uploadFileReplyFinished );

  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
}

void MerginApi::uploadStart( const QString &projectFullName, const QByteArray &json )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return;
  }

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  QNetworkRequest request;
  QUrl url( mApiRoot + QStringLiteral( "v1/project/push/%1" ).arg( projectFullName ) );
  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );
  request.setRawHeader( "Content-Type", "application/json" );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ), projectFullName );

  Q_ASSERT( !transaction.replyUploadStart );
  transaction.replyUploadStart = mManager.post( request, json );
  connect( transaction.replyUploadStart, &QNetworkReply::finished, this, &MerginApi::uploadStartReplyFinished );

  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
}

void MerginApi::uploadCancel( const QString &projectFullName )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return;
  }

  if ( !mTransactionalStatus.contains( projectFullName ) )
    return;

  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  // There is an open transaction, abort it followed by calling cancelUpload again.
  if ( transaction.replyUploadProjectInfo )
  {
    InputUtils::log( transaction.replyUploadProjectInfo->url().toString(), QStringLiteral( "ABORT" ) );
    transaction.replyUploadProjectInfo->abort();  // will trigger uploadInfoReplyFinished slot and emit sync finished
  }
  else if ( transaction.replyUploadStart )
  {
    InputUtils::log( transaction.replyUploadStart->url().toString(), QStringLiteral( "ABORT" ) );
    transaction.replyUploadStart->abort();  // will trigger uploadStartReplyFinished slot and emit sync finished
  }
  else if ( transaction.replyUploadFile )
  {
    QString transactionUUID = transaction.transactionUUID;  // copy transaction uuid as the transaction object will be gone after abort
    InputUtils::log( transaction.replyUploadFile->url().toString(), QStringLiteral( "ABORT" ) );
    transaction.replyUploadFile->abort();  // will trigger uploadFileReplyFinished slot and emit sync finished

    // also need to cancel the transaction
    sendUploadCancelRequest( projectFullName, transactionUUID );
  }
  else if ( transaction.replyUploadFinish )
  {
    QString transactionUUID = transaction.transactionUUID;  // copy transaction uuid as the transaction object will be gone after abort
    InputUtils::log( transaction.replyUploadFinish->url().toString(), QStringLiteral( "ABORT" ) );
    transaction.replyUploadFinish->abort();  // will trigger uploadFinishReplyFinished slot and emit sync finished

    sendUploadCancelRequest( projectFullName, transactionUUID );
  }
  else
  {
    Q_ASSERT( false );  // unexpected state
  }
}


void MerginApi::sendUploadCancelRequest( const QString &projectFullName, const QString &transactionUUID )
{
  QNetworkRequest request;
  QUrl url( mApiRoot + QStringLiteral( "v1/project/push/cancel/%1" ).arg( transactionUUID ) );
  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );
  request.setRawHeader( "Content-Type", "application/json" );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ), projectFullName );

  QNetworkReply *reply = mManager.post( request, QByteArray() );
  connect( reply, &QNetworkReply::finished, this, &MerginApi::uploadCancelReplyFinished );
  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
}

void MerginApi::updateCancel( const QString &projectFullName )
{
  if ( !mTransactionalStatus.contains( projectFullName ) )
    return;

  InputUtils::log( projectFullName, "updateCancel" );

  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  if ( transaction.replyProjectInfo )
  {
    // we're still fetching project info
    InputUtils::log( transaction.replyProjectInfo->url().toString(), QStringLiteral( "ABORT" ) );
    transaction.replyProjectInfo->abort();  // abort will trigger updateInfoReplyFinished() slot
  }
  else if ( transaction.replyDownloadFile )
  {
    // we're already downloading some files
    InputUtils::log( transaction.replyDownloadFile->url().toString(), QStringLiteral( "ABORT" ) );
    transaction.replyDownloadFile->abort();  // abort will trigger downloadFileReplyFinished slot
  }
  else
  {
    Q_ASSERT( false );  // unexpected state
  }
}


void MerginApi::uploadFinish( const QString &projectFullName, const QString &transactionUUID )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return;
  }

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  QNetworkRequest request;
  QUrl url( mApiRoot + QStringLiteral( "v1/project/push/finish/%1" ).arg( transactionUUID ) );
  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );
  request.setRawHeader( "Content-Type", "application/json" );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ), projectFullName );

  Q_ASSERT( !transaction.replyUploadFinish );
  transaction.replyUploadFinish = mManager.post( request, QByteArray() );
  connect( transaction.replyUploadFinish, &QNetworkReply::finished, this, &MerginApi::uploadFinishReplyFinished );

  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
}

void MerginApi::updateProject( const QString &projectNamespace, const QString &projectName )
{
  QString projectFullName = getFullProjectName( projectNamespace, projectName );
  QNetworkReply *reply = getProjectInfo( projectFullName );
  if ( reply )
  {
    Q_ASSERT( !mTransactionalStatus.contains( projectFullName ) );
    mTransactionalStatus.insert( projectFullName, TransactionStatus() );
    mTransactionalStatus[projectFullName].replyProjectInfo = reply;

    emit syncProjectStatusChanged( projectFullName, 0 );

    connect( reply, &QNetworkReply::finished, this, &MerginApi::updateInfoReplyFinished );
  }
}

void MerginApi::uploadProject( const QString &projectNamespace, const QString &projectName )
{
  QString projectFullName = getFullProjectName( projectNamespace, projectName );

  // create entry about pending upload for the project
  Q_ASSERT( !mTransactionalStatus.contains( projectFullName ) );
  mTransactionalStatus.insert( projectFullName, TransactionStatus() );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  transaction.replyUploadProjectInfo = getProjectInfo( projectFullName );
  if ( transaction.replyUploadProjectInfo )
  {
    emit syncProjectStatusChanged( projectFullName, 0 );

    connect( transaction.replyUploadProjectInfo, &QNetworkReply::finished, this, &MerginApi::uploadInfoReplyFinished );
  }
}

void MerginApi::authorize( const QString &login, const QString &password )
{
  mPassword = password;

  QNetworkRequest request;
  QString urlString = mApiRoot + QStringLiteral( "v1/auth/login" );
  QUrl url( urlString );
  request.setUrl( url );
  request.setRawHeader( "Content-Type", "application/json" );

  QJsonDocument jsonDoc;
  QJsonObject jsonObject;
  jsonObject.insert( QStringLiteral( "login" ), login );
  jsonObject.insert( QStringLiteral( "password" ), mPassword );
  jsonDoc.setObject( jsonObject );
  QByteArray json = jsonDoc.toJson( QJsonDocument::Compact );

  QNetworkReply *reply = mManager.post( request, json );
  connect( reply, &QNetworkReply::finished, this, &MerginApi::authorizeFinished );
  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
}

void MerginApi::getUserInfo( const QString &username )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return;
  }

  QNetworkRequest request;
  QString urlString = mApiRoot + QStringLiteral( "v1/user/" ) + username;
  QUrl url( urlString );
  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );

  QNetworkReply *reply = mManager.get( request );
  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
  connect( reply, &QNetworkReply::finished, this, &MerginApi::getUserInfoFinished );
}

void MerginApi::clearAuth()
{
  mUsername = "";
  mPassword = "";
  mAuthToken.clear();
  mTokenExpiration.setTime( QTime() );
  mUserId = -1;
  mDiskUsage = 0;
  mStorageLimit = 0;
  emit authChanged();
}

void MerginApi::resetApiRoot()
{
  QSettings settings;
  settings.beginGroup( QStringLiteral( "Input/" ) );
  setApiRoot( defaultApiRoot() );
  settings.endGroup();
}

bool MerginApi::hasAuthData()
{
  return !mUsername.isEmpty() && !mPassword.isEmpty();
}

void MerginApi::createProject( const QString &projectNamespace, const QString &projectName )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return;
  }

  QNetworkRequest request;
  QUrl url( mApiRoot + QString( "/v1/project/%1" ).arg( projectNamespace ) );
  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );
  request.setRawHeader( "Content-Type", "application/json" );
  request.setRawHeader( "Accept", "application/json" );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ), getFullProjectName( projectNamespace, projectName ) );

  QJsonDocument jsonDoc;
  QJsonObject jsonObject;
  jsonObject.insert( QStringLiteral( "name" ), projectName );
  jsonObject.insert( QStringLiteral( "public" ), false );
  jsonDoc.setObject( jsonObject );
  QByteArray json = jsonDoc.toJson( QJsonDocument::Compact );

  QNetworkReply *reply = mManager.post( request, json );
  connect( reply, &QNetworkReply::finished, this, &MerginApi::createProjectFinished );
  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
}

void MerginApi::deleteProject( const QString &projectNamespace, const QString &projectName )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return;
  }

  QNetworkRequest request;
  QUrl url( mApiRoot + QStringLiteral( "/v1/project/%1/%2" ).arg( projectNamespace ).arg( projectName ) );
  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ), getFullProjectName( projectNamespace, projectName ) );
  QNetworkReply *reply = mManager.deleteResource( request );
  connect( reply, &QNetworkReply::finished, this, &MerginApi::deleteProjectFinished );
  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
}

void MerginApi::clearTokenData()
{
  mTokenExpiration = QDateTime().currentDateTime().addDays( -42 ); // to make it expired arbitrary days ago
  mAuthToken.clear();
}


void MerginApi::saveAuthData()
{
  QSettings settings;
  settings.beginGroup( "Input/" );
  settings.setValue( "username", mUsername );
  settings.setValue( "password", mPassword );
  settings.setValue( "userId", mUserId );
  settings.setValue( "token", mAuthToken );
  settings.setValue( "expire", mTokenExpiration );
  settings.setValue( "apiRoot", mApiRoot );
  settings.endGroup();
}

void MerginApi::createProjectFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  QString projectFullName = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ) ).toString();

  if ( r->error() == QNetworkReply::NoError )
  {
    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );
    emit notify( QStringLiteral( "Project created" ) );
    emit projectCreated( projectFullName, true );
  }
  else
  {
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    QString message = QStringLiteral( "FAILED - %1: %2" ).arg( r->errorString(), serverMsg );
    InputUtils::log( r->url().toString(), message );
    emit projectCreated( projectFullName, false );
    emit networkErrorOccurred( serverMsg, QStringLiteral( "Mergin API error: createProject" ) );
  }
  r->deleteLater();
}

void MerginApi::deleteProjectFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  QString projectFullName = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ) ).toString();

  if ( r->error() == QNetworkReply::NoError )
  {
    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );

    emit notify( QStringLiteral( "Project deleted" ) );
    emit serverProjectDeleted( projectFullName, true );
  }
  else
  {
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1. %2" ).arg( r->errorString(), serverMsg ) );
    emit serverProjectDeleted( projectFullName, false );
    emit networkErrorOccurred( serverMsg, QStringLiteral( "Mergin API error: deleteProject" ) );
  }
  r->deleteLater();
}

void MerginApi::authorizeFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  if ( r->error() == QNetworkReply::NoError )
  {
    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );
    QJsonDocument doc = QJsonDocument::fromJson( r->readAll() );
    if ( doc.isObject() )
    {
      QJsonObject docObj = doc.object();
      QJsonObject session = docObj.value( QStringLiteral( "session" ) ).toObject();
      mAuthToken = session.value( QStringLiteral( "token" ) ).toString().toUtf8();
      mTokenExpiration = QDateTime::fromString( session.value( QStringLiteral( "expire" ) ).toString(), Qt::ISODateWithMs ).toUTC();
      mUserId = docObj.value( QStringLiteral( "id" ) ).toInt();
      mDiskUsage = docObj.value( QStringLiteral( "disk_usage" ) ).toInt();
      mStorageLimit = docObj.value( QStringLiteral( "storage_limit" ) ).toInt();
      mUsername = docObj.value( QStringLiteral( "username" ) ).toString();
    }
    emit authChanged();
  }
  else
  {
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1. %2" ).arg( r->errorString(), serverMsg ) );
    QVariant statusCode = r->attribute( QNetworkRequest::HttpStatusCodeAttribute );
    int status = statusCode.toInt();
    if ( status == 401 || status == 400 )
    {
      emit authFailed();
      emit notify( serverMsg );
    }
    else
    {
      emit networkErrorOccurred( serverMsg, QStringLiteral( "Mergin API error: authorize" ) );
    }
    mUsername.clear();
    mPassword.clear();
    clearTokenData();
  }
  if ( mAuthLoopEvent.isRunning() )
  {
    mAuthLoopEvent.exit();
  }
  r->deleteLater();
}

void MerginApi::pingMerginReplyFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );
  QString apiVersion;
  QString serverMsg;

  if ( r->error() == QNetworkReply::NoError )
  {
    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );
    QJsonDocument doc = QJsonDocument::fromJson( r->readAll() );
    if ( doc.isObject() )
    {
      QJsonObject obj = doc.object();
      apiVersion = obj.value( QStringLiteral( "version" ) ).toString();
    }
  }
  else
  {
    serverMsg = extractServerErrorMsg( r->readAll() );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1. %2" ).arg( r->errorString(), serverMsg ) );
  }
  r->deleteLater();
  emit pingMerginFinished( apiVersion, serverMsg );
}


QNetworkReply *MerginApi::getProjectInfo( const QString &projectFullName )
{
  if ( !validateAuthAndContinute() || mApiVersionStatus != MerginApiStatus::OK )
  {
    return nullptr;
  }

  QNetworkRequest request;
  QUrl url( mApiRoot + QStringLiteral( "/v1/project/%1" ).arg( projectFullName ) );

  request.setUrl( url );
  request.setRawHeader( "Authorization", QByteArray( "Bearer " + mAuthToken ) );
  request.setAttribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ), projectFullName );

  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
  return mManager.get( request );
}

void MerginApi::loadAuthData()
{
  QSettings settings;
  settings.beginGroup( QStringLiteral( "Input/" ) );
  setApiRoot( settings.value( QStringLiteral( "apiRoot" ) ).toString() );
  mUsername = settings.value( QStringLiteral( "username" ) ).toString();
  mPassword = settings.value( QStringLiteral( "password" ) ).toString();
  mUserId = settings.value( QStringLiteral( "userId" ) ).toInt();
  mTokenExpiration = settings.value( QStringLiteral( "expire" ) ).toDateTime();
  mAuthToken = settings.value( QStringLiteral( "token" ) ).toByteArray();
}

bool MerginApi::validateAuthAndContinute()
{
  if ( !hasAuthData() )
  {
    emit authRequested();
    return false;
  }

  if ( mAuthToken.isEmpty() || mTokenExpiration < QDateTime().currentDateTime().toUTC() )
  {
    authorize( mUsername, mPassword );

    mAuthLoopEvent.exec();
  }
  return true;
}

void MerginApi::checkMerginVersion( QString apiVersion, QString msg )
{
  if ( msg.isEmpty() )
  {
    int major = -1;
    int minor = -1;
    QRegularExpression re;
    re.setPattern( QStringLiteral( "(?<major>\\d+)[.](?<minor>\\d+)" ) );
    QRegularExpressionMatch match = re.match( apiVersion );
    if ( match.hasMatch() )
    {
      major = match.captured( "major" ).toInt();
      minor = match.captured( "minor" ).toInt();
    }

    if ( ( MERGIN_API_VERSION_MAJOR == major && MERGIN_API_VERSION_MINOR <= minor ) || ( MERGIN_API_VERSION_MAJOR < major ) )
    {
      setApiVersionStatus( MerginApiStatus::OK );
    }
    else
    {
      setApiVersionStatus( MerginApiStatus::INCOMPATIBLE );
    }
  }
  else
  {
    setApiVersionStatus( MerginApiStatus::NOT_FOUND );
  }

  // TODO remove, only for te4eting
  setApiVersionStatus( MerginApiStatus::OK );
}

bool MerginApi::extractProjectName( const QString &sourceString, QString &projectNamespace, QString &name )
{
  QStringList parts = sourceString.split( "/" );
  if ( parts.length() > 1 )
  {
    projectNamespace = parts.at( parts.length() - 2 );
    name = parts.last();
    return true;
  }
  else
  {
    name = sourceString;
    return false;
  }
}

QString MerginApi::extractServerErrorMsg( const QByteArray &data )
{
  QString serverMsg;
  QJsonDocument doc = QJsonDocument::fromJson( data );
  if ( doc.isObject() )
  {
    QJsonObject obj = doc.object();
    QJsonValue vDetail = obj.value( "detail" );
    if ( vDetail.isString() )
    {
      serverMsg = vDetail.toString();
    }
    else if ( vDetail.isObject() )
    {
      serverMsg = QJsonDocument( vDetail.toObject() ).toJson();
    }
    else
    {
      serverMsg = "[can't parse server error]";
    }
  }
  else
  {
    serverMsg = data;
  }

  return serverMsg;
}


LocalProjectInfo MerginApi::getLocalProject( const QString &projectFullName )
{
  return mLocalProjects.projectFromMerginName( projectFullName );
}

QString MerginApi::findUniqueProjectDirectoryName( QString path )
{
  QDir projectDir( path );
  if ( projectDir.exists() )
  {
    int i = 0;
    QFileInfo info( path + QString::number( i ) );
    while ( info.exists() && info.isDir() )
    {
      ++i;
      info.setFile( path + QString::number( i ) );
    }
    return path + QString::number( i );
  }
  else
  {
    return path;
  }
}

QString MerginApi::createUniqueProjectDirectory( const QString &projectName )
{
  QString projectDirPath = findUniqueProjectDirectoryName( mDataDir + "/" + projectName );
  QDir projectDir( projectDirPath );
  if ( !projectDir.exists() )
  {
    QDir dir( "" );
    dir.mkdir( projectDirPath );
  }
  return projectDirPath;
}

QString MerginApi::getTempProjectDir( const QString &projectFullName )
{
  return mDataDir + "/" + TEMP_FOLDER + projectFullName;
}

QString MerginApi::getFullProjectName( QString projectNamespace, QString projectName )
{
  return QString( "%1/%2" ).arg( projectNamespace ).arg( projectName );
}

MerginApiStatus::VersionStatus MerginApi::apiVersionStatus() const
{
  return mApiVersionStatus;
}

void MerginApi::setApiVersionStatus( const MerginApiStatus::VersionStatus &apiVersionStatus )
{
  mApiVersionStatus = apiVersionStatus;
  emit apiVersionStatusChanged();
}

int MerginApi::userId() const
{
  return mUserId;
}

void MerginApi::setUserId( int userId )
{
  mUserId = userId;
}

int MerginApi::storageLimit() const
{
  return mStorageLimit;
}

int MerginApi::diskUsage() const
{
  return mDiskUsage;
}

void MerginApi::pingMergin()
{
  if ( mApiVersionStatus == MerginApiStatus::OK ) return;

  setApiVersionStatus( MerginApiStatus::PENDING );

  QNetworkRequest request;
  QUrl url( mApiRoot + QStringLiteral( "/ping" ) );
  request.setUrl( url );

  QNetworkReply *reply = mManager.get( request );
  InputUtils::log( url.toString(), QStringLiteral( "STARTED" ) );
  connect( reply, &QNetworkReply::finished, this, &MerginApi::pingMerginReplyFinished );
}

QString MerginApi::apiRoot() const
{
  return mApiRoot;
}

void MerginApi::setApiRoot( const QString &apiRoot )
{
  QSettings settings;
  settings.beginGroup( QStringLiteral( "Input/" ) );
  if ( apiRoot.isEmpty() )
  {
    mApiRoot = defaultApiRoot();
  }
  else
  {
    mApiRoot = apiRoot;
  }
  settings.setValue( QStringLiteral( "apiRoot" ), mApiRoot );
  settings.endGroup();
  setApiVersionStatus( MerginApiStatus::UNKNOWN );
  emit apiRootChanged();
}

QString MerginApi::username() const
{
  return mUsername;
}

MerginProjectList MerginApi::projects()
{
  return mRemoteProjects;
}

QList<MerginFile> MerginApi::getLocalProjectFiles( const QString &projectPath )
{
  QList<MerginFile> merginFiles;
  QSet<QString> localFiles = listFiles( projectPath );
  for ( QString p : localFiles )
  {

    MerginFile file;
    QByteArray localChecksumBytes = getChecksum( projectPath + p );
    QString localChecksum = QString::fromLatin1( localChecksumBytes.data(), localChecksumBytes.size() );
    file.checksum = localChecksum;
    file.path = p;
    QFileInfo info( projectPath + p );
    file.size = info.size();
    file.mtime = info.lastModified();
    merginFiles.append( file );
  }
  return merginFiles;
}

void MerginApi::listProjectsReplyFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  if ( r->error() == QNetworkReply::NoError )
  {
    QByteArray data = r->readAll();
    mRemoteProjects = parseListProjectsMetadata( data );

    // for any local projects we can update the latest server version
    for ( MerginProjectListEntry project : mRemoteProjects )
    {
      LocalProjectInfo localProject = mLocalProjects.projectFromMerginName( getFullProjectName( project.projectNamespace, project.projectName ) );
      if ( localProject.isValid() )
      {
        mLocalProjects.updateMerginServerVersion( localProject.projectDir, project.version );
      }
    }

    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );
  }
  else
  {
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    QString message = QStringLiteral( "Network API error: %1(): %2. %3" ).arg( QStringLiteral( "listProjects" ), r->errorString(), serverMsg );
    emit networkErrorOccurred( serverMsg, QStringLiteral( "Mergin API error: listProjects" ) );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1" ).arg( message ) );
    mRemoteProjects.clear();

    emit listProjectsFailed();
  }

  r->deleteLater();
  emit listProjectsFinished( mRemoteProjects );
}

void MerginApi::takeFirstAndDownload( const QString &projectFullName, const QString &version )
{
  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  MerginFile nextFile = mTransactionalStatus[projectFullName].files.first();
  if ( !nextFile.size )
  {
    createEmptyFile( getTempProjectDir( projectFullName ) + "/" + nextFile.path );
    continueDownloadFiles( projectFullName, version, 0 );
  }
  else
  {
    downloadFile( projectFullName, nextFile.path, version, 0 );
  }
}

void MerginApi::continueDownloadFiles( const QString &projectFullName, const QString &version, int lastChunkNo )
{
  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  Q_ASSERT( !transaction.files.isEmpty() );
  MerginFile currentFile = transaction.files.first();
  if ( lastChunkNo + 1 <= currentFile.chunks.size() - 1 )
  {
    downloadFile( projectFullName, currentFile.path, version, lastChunkNo + 1 );
  }
  else
  {
    transaction.files.removeFirst();
    if ( !transaction.files.isEmpty() )
    {
      takeFirstAndDownload( projectFullName, version );
    }
    else
    {
      finalizeProjectUpdate( projectFullName );
    }
  }
}

void MerginApi::finalizeProjectUpdate( const QString &projectFullName )
{
  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  QString projectDir = transaction.projectDir;

  // rename local conflicting files that were updated when also the server got updated
  for ( QString filePath : transaction.diff.conflictRemoteUpdatedLocalUpdated )
  {
    InputUtils::log( projectFullName, "conflicting remote update/local update: " + filePath );
    QString origPath = projectDir + "/" + filePath;
    if ( !QFile::rename( origPath, origPath + "_conflict" ) )
    {
      InputUtils::log( projectFullName, "failed rename of conflicting file: " + filePath );
    }
  }

  // rename local conflicting files that were added when also the server got those files added
  for ( QString filePath : transaction.diff.conflictRemoteAddedLocalAdded )
  {
    InputUtils::log( projectFullName, "conflicting remote add/local add: " + filePath );
    QString origPath = projectDir + "/" + filePath;
    if ( !QFile::rename( origPath, origPath + "_conflict" ) )
    {
      InputUtils::log( projectFullName, "failed rename of conflicting file: " + filePath );
    }
  }

  copyTempFilesToProject( projectDir, projectFullName );

  // remove files that have been removed from the server
  for ( QString filename : transaction.diff.remoteDeleted )
  {
    QFile file( projectDir + "/" + filename );
    file.remove();
  }

  // add the local project if not there yet
  if ( !mLocalProjects.projectFromMerginName( projectFullName ).isValid() )
  {
    QString projectNamespace, projectName;
    extractProjectName( projectFullName, projectNamespace, projectName );
    mLocalProjects.addMerginProject( projectDir, projectNamespace, projectName );
  }

  finishProjectSync( projectFullName, true );
}


void MerginApi::downloadFileReplyFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  QString projectFullName = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ) ).toString();

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];
  Q_ASSERT( r == transaction.replyDownloadFile );

  QUrlQuery query( r->url().query() );
  QString filename = query.queryItemValue( "file" );
  QString version = query.queryItemValue( "version" );
  int chunkNo = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrChunkNo ) ).toInt();

  if ( r->error() == QNetworkReply::NoError )
  {
    bool overwrite = true; // chunkNo == 0
    bool closeFile = false;

    QList<MerginFile> files = transaction.files;
    if ( !files.isEmpty() )
    {
      MerginFile file = transaction.files.first();
      overwrite  = file.chunks.size() <= 1;

      if ( chunkNo == file.chunks.size() - 1 )
      {
        closeFile = true;
      }
    }

    QString tempFoler = getTempProjectDir( projectFullName );
    createPathIfNotExists( tempFoler );
    QByteArray data = r->readAll();
    handleOctetStream( data, tempFoler, filename, closeFile, overwrite );
    transaction.transferedSize += data.size();

    emit syncProjectStatusChanged( projectFullName, transaction.transferedSize / transaction.totalSize );

    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );

    transaction.replyDownloadFile->deleteLater();
    transaction.replyDownloadFile = nullptr;

    // Send another request afterwards
    continueDownloadFiles( projectFullName, version, chunkNo );
  }
  else
  {
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    if ( serverMsg.isEmpty() )
    {
      serverMsg = r->errorString();
    }
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1. %2" ).arg( r->errorString(), serverMsg ) );

    transaction.replyDownloadFile->deleteLater();
    transaction.replyDownloadFile = nullptr;

    // get rid of the temporary download dir where we may have left some downloaded files
    QDir( getTempProjectDir( projectFullName ) ).removeRecursively();

    if ( transaction.firstTimeDownload )
    {
      Q_ASSERT( !transaction.projectDir.isEmpty() );
      QDir( transaction.projectDir ).removeRecursively();
    }

    finishProjectSync( projectFullName, false );

    emit networkErrorOccurred( serverMsg, QStringLiteral( "Mergin API error: downloadFile" ) );
  }
}

void MerginApi::uploadStartReplyFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  QString projectFullName = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ) ).toString();

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];
  Q_ASSERT( r == transaction.replyUploadStart );

  if ( r->error() == QNetworkReply::NoError )
  {
    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );
    QByteArray data = r->readAll();

    transaction.replyUploadStart->deleteLater();
    transaction.replyUploadStart = nullptr;

    QList<MerginFile> files = transaction.files;
    if ( !files.isEmpty() )
    {
      QString transactionUUID;
      QJsonDocument doc = QJsonDocument::fromJson( data );
      if ( doc.isObject() )
      {
        QJsonObject docObj = doc.object();
        transactionUUID = docObj.value( QStringLiteral( "transaction" ) ).toString();
        transaction.transactionUUID = transactionUUID;
      }

      MerginFile file = files.first();
      uploadFile( projectFullName, transactionUUID, file );
      emit pushFilesStarted();
    }
    else  // pushing only files to be removed
    {
      // we are done here - no upload of chunks, no request to "finish"
      // because server immediatelly creates a new version without starting a transaction to upload chunks

      transaction.projectMetadata = data;
      transaction.version = MerginProjectMetadata::fromJson( data ).version;

      finishProjectSync( projectFullName, true );
    }
  }
  else
  {
    QVariant statusCode = r->attribute( QNetworkRequest::HttpStatusCodeAttribute );
    int status = statusCode.toInt();
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    QString errorMsg = r->errorString();
    bool showAsDialog = status == 400 && serverMsg == QStringLiteral( "You have reached a data limit" );

    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1. %2" ).arg( r->errorString(), serverMsg ) );

    transaction.replyUploadStart->deleteLater();
    transaction.replyUploadStart = nullptr;

    emit networkErrorOccurred( serverMsg, QStringLiteral( "Mergin API error: uploadStartReply" ), showAsDialog );
    finishProjectSync( projectFullName, false );
  }
}

void MerginApi::uploadFileReplyFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  QString projectFullName = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ) ).toString();

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];
  Q_ASSERT( r == transaction.replyUploadFile );

  QStringList params = ( r->url().toString().split( "/" ) );
  QString transactionUUID = params.at( params.length() - 2 );
  QString chunkID = params.at( params.length() - 1 );
  Q_ASSERT( transactionUUID == transaction.transactionUUID );

  if ( r->error() == QNetworkReply::NoError )
  {
    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );

    transaction.replyUploadFile->deleteLater();
    transaction.replyUploadFile = nullptr;

    MerginFile currentFile = transaction.files.first();
    int chunkNo = currentFile.chunks.indexOf( chunkID );
    if ( chunkNo < currentFile.chunks.size() - 1 )
    {
      uploadFile( projectFullName, transactionUUID, currentFile, chunkNo + 1 );
    }
    else
    {
      transaction.transferedSize += currentFile.size;

      emit syncProjectStatusChanged( projectFullName, transaction.transferedSize / transaction.totalSize );
      transaction.files.removeFirst();

      if ( !transaction.files.isEmpty() )
      {
        MerginFile nextFile = transaction.files.first();
        uploadFile( projectFullName, transactionUUID, nextFile );
      }
      else
      {
        uploadFinish( projectFullName, transactionUUID );
      }
    }
  }
  else
  {
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1. %2" ).arg( r->errorString(), serverMsg ) );
    emit networkErrorOccurred( serverMsg, QStringLiteral( "Mergin API error: uploadFile" ) );

    transaction.replyUploadFile->deleteLater();
    transaction.replyUploadFile = nullptr;

    finishProjectSync( projectFullName, false );
  }
}

void MerginApi::updateInfoReplyFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  QString projectFullName = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ) ).toString();

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];
  Q_ASSERT( r == transaction.replyProjectInfo );

  if ( r->error() == QNetworkReply::NoError )
  {
    QString url = r->url().toString();
    InputUtils::log( url, QStringLiteral( "FINISHED" ) );
    QByteArray data = r->readAll();

    transaction.replyProjectInfo->deleteLater();
    transaction.replyProjectInfo = nullptr;

    startProjectUpdate( projectFullName, data );
  }
  else
  {
    QString message = QStringLiteral( "Network API error: %1(): %2" ).arg( QStringLiteral( "projectInfo" ), r->errorString() );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1" ).arg( message ) );

    transaction.replyProjectInfo->deleteLater();
    transaction.replyProjectInfo = nullptr;

    finishProjectSync( projectFullName, false );
  }
}

void MerginApi::startProjectUpdate( const QString &projectFullName, const QByteArray &data )
{
  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  LocalProjectInfo projectInfo = mLocalProjects.projectFromMerginName( projectFullName );
  if ( projectInfo.isValid() )
  {
    transaction.projectDir = projectInfo.projectDir;
  }
  else
  {
    QString projectNamespace;
    QString projectName;
    extractProjectName( projectFullName, projectNamespace, projectName );

    // project has not been downloaded yet - we need to create a directory for it
    transaction.projectDir = createUniqueProjectDirectory( projectName );
    transaction.firstTimeDownload = true;
  }

  Q_ASSERT( !transaction.projectDir.isEmpty() );  // that would mean we do not have entry -> fail getting local files

  QList<MerginFile> localFiles = getLocalProjectFiles( transaction.projectDir + "/" );
  MerginProjectMetadata serverProject = MerginProjectMetadata::fromJson( data );
  MerginProjectMetadata oldServerProject = MerginProjectMetadata::fromCachedJson( transaction.projectDir + "/" + sMetadataFile );

  transaction.projectMetadata = data;
  transaction.version = serverProject.version;
  transaction.diff = compareProjectFiles( oldServerProject.files, serverProject.files, localFiles );
  InputUtils::log( "update", transaction.diff.dump() );

  QList<MerginFile> filesToDownload;
  qint64 totalSize = 0;
  for ( QString filePath : transaction.diff.remoteAdded )
  {
    MerginFile file = serverProject.fileInfo( filePath );
    file.chunks = generateChunkIdsForSize( file.size ); // doesnt really matter whats there, only how many chunks are expected
    filesToDownload << file;
    totalSize += file.size;
  }

  for ( QString filePath : transaction.diff.remoteUpdated )
  {
    MerginFile file = serverProject.fileInfo( filePath );
    file.chunks = generateChunkIdsForSize( file.size ); // doesnt really matter whats there, only how many chunks are expected
    filesToDownload << file;
    totalSize += file.size;
  }

  // also download files which were changed both on the server and locally (the local version will be renamed as conflicting copy)
  for ( QString filePath : transaction.diff.conflictRemoteUpdatedLocalUpdated )
  {
    MerginFile file = serverProject.fileInfo( filePath );
    file.chunks = generateChunkIdsForSize( file.size ); // doesnt really matter whats there, only how many chunks are expected
    filesToDownload << file;
    totalSize += file.size;
  }

  // also download files which were added both on the server and locally (the local version will be renamed as conflicting copy)
  for ( QString filePath : transaction.diff.conflictRemoteAddedLocalAdded )
  {
    MerginFile file = serverProject.fileInfo( filePath );
    file.chunks = generateChunkIdsForSize( file.size ); // doesnt really matter whats there, only how many chunks are expected
    filesToDownload << file;
    totalSize += file.size;
  }

  transaction.totalSize = totalSize;
  transaction.files = filesToDownload;

  if ( !filesToDownload.isEmpty() )
  {
    takeFirstAndDownload( projectFullName, QString( "v%1" ).arg( serverProject.version ) );
    emit pullFilesStarted();
  }
  else
  {
    // there's nothing to download so just finalize the update
    finalizeProjectUpdate( projectFullName );
  }
}


static MerginFile findFile( const QString &filePath, const QList<MerginFile> &files )
{
  for ( const MerginFile &merginFile : files )
  {
    if ( merginFile.path == filePath )
      return merginFile;
  }
  qDebug() << "requested findFile() for non-existant file! " << filePath;
  return MerginFile();
}


void MerginApi::uploadInfoReplyFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  QString projectFullName = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ) ).toString();

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];
  Q_ASSERT( r == transaction.replyUploadProjectInfo );

  if ( r->error() == QNetworkReply::NoError )
  {
    QString url = r->url().toString();
    InputUtils::log( url, QStringLiteral( "FINISHED" ) );
    QByteArray data = r->readAll();

    transaction.replyUploadProjectInfo->deleteLater();
    transaction.replyUploadProjectInfo = nullptr;

    LocalProjectInfo projectInfo = mLocalProjects.projectFromMerginName( projectFullName );
    transaction.projectDir = projectInfo.projectDir;
    Q_ASSERT( !transaction.projectDir.isEmpty() );

    MerginProjectMetadata serverProject = MerginProjectMetadata::fromJson( data );
    // get the latest server version from our reply (we do not update it in LocalProjectsManager though... I guess we don't need to)
    projectInfo.serverVersion = serverProject.version;

    // now let's figure a key question: are we on the most recent version of the project
    // if we're about to do upload? because if not, we need to do local update first
    if ( projectInfo.isValid() && projectInfo.localVersion != -1 && projectInfo.localVersion < projectInfo.serverVersion )
    {
      transaction.updateBeforeUpload = true;
      startProjectUpdate( projectFullName, data );
      return;
    }

    QList<MerginFile> localFiles = getLocalProjectFiles( transaction.projectDir + "/" );
    MerginProjectMetadata oldServerProject = MerginProjectMetadata::fromCachedJson( transaction.projectDir + "/" + sMetadataFile );

    mLocalProjects.updateMerginServerVersion( transaction.projectDir, serverProject.version );

    transaction.diff = compareProjectFiles( oldServerProject.files, serverProject.files, localFiles );
    InputUtils::log( url, transaction.diff.dump() );

    // TODO: make sure there are no remote files to add/update/remove nor conflicts

    QList<MerginFile> filesToUpload;
    QList<MerginFile> addedMerginFiles, updatedMerginFiles, deletedMerginFiles;
    for ( QString filePath : transaction.diff.localAdded )
    {
      MerginFile merginFile = findFile( filePath, localFiles );
      merginFile.chunks = generateChunkIdsForSize( merginFile.size );
      addedMerginFiles.append( merginFile );
    }
    for ( QString filePath : transaction.diff.localUpdated )
    {
      MerginFile merginFile = findFile( filePath, localFiles );
      merginFile.chunks = generateChunkIdsForSize( merginFile.size );
      updatedMerginFiles.append( merginFile );
    }
    for ( QString filePath : transaction.diff.localDeleted )
    {
      MerginFile merginFile = findFile( filePath, serverProject.files );
      deletedMerginFiles.append( merginFile );
    }

    QJsonArray added = prepareUploadChangesJSON( addedMerginFiles );
    filesToUpload.append( addedMerginFiles );

    QJsonArray modified = prepareUploadChangesJSON( updatedMerginFiles );
    filesToUpload.append( updatedMerginFiles );

    QJsonArray removed = prepareUploadChangesJSON( deletedMerginFiles );
    // removed not in filesToUpload

    QJsonObject changes;
    changes.insert( "added", added );
    changes.insert( "removed", removed );
    changes.insert( "updated", modified );
    changes.insert( "renamed", QJsonArray() );

    qint64 totalSize = 0;
    for ( MerginFile file : filesToUpload )
    {
      totalSize += file.size;
    }

    transaction.totalSize = totalSize;
    transaction.files = filesToUpload;

    QJsonObject json;
    json.insert( QStringLiteral( "changes" ), changes );
    json.insert( QStringLiteral( "version" ), QString( "v%1" ).arg( serverProject.version ) );
    QJsonDocument jsonDoc;
    jsonDoc.setObject( json );

    uploadStart( projectFullName, jsonDoc.toJson( QJsonDocument::Compact ) );
  }
  else
  {
    QString message = QStringLiteral( "Network API error: %1(): %2" ).arg( QStringLiteral( "projectInfo" ), r->errorString() );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1" ).arg( message ) );

    transaction.replyUploadProjectInfo->deleteLater();
    transaction.replyUploadProjectInfo = nullptr;

    finishProjectSync( projectFullName, false );
  }
}

void MerginApi::uploadFinishReplyFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  QString projectFullName = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ) ).toString();

  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];
  Q_ASSERT( r == transaction.replyUploadFinish );

  if ( r->error() == QNetworkReply::NoError )
  {
    Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
    QByteArray data = r->readAll();
    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );

    transaction.replyUploadFinish->deleteLater();
    transaction.replyUploadFinish = nullptr;

    transaction.projectMetadata = data;
    transaction.version = MerginProjectMetadata::fromJson( data ).version;

    finishProjectSync( projectFullName, true );
  }
  else
  {
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    QString message = QStringLiteral( "Network API error: %1(): %2. %3" ).arg( QStringLiteral( "uploadFinish" ), r->errorString(), serverMsg );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1" ).arg( message ) );

    transaction.replyUploadFinish->deleteLater();
    transaction.replyUploadFinish = nullptr;

    finishProjectSync( projectFullName, false );
  }
}

void MerginApi::uploadCancelReplyFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  QString projectFullName = r->request().attribute( static_cast<QNetworkRequest::Attribute>( AttrProjectFullName ) ).toString();

  if ( r->error() == QNetworkReply::NoError )
  {
    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );
  }
  else
  {
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    QString message = QStringLiteral( "Network API error: %1(): %2. %3" ).arg( QStringLiteral( "uploadCancel" ), r->errorString(), serverMsg );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1" ).arg( message ) );
  }

  r->deleteLater();
}

void MerginApi::getUserInfoFinished()
{
  QNetworkReply *r = qobject_cast<QNetworkReply *>( sender() );
  Q_ASSERT( r );

  if ( r->error() == QNetworkReply::NoError )
  {
    InputUtils::log( r->url().toString(), QStringLiteral( "FINISHED" ) );
    QJsonDocument doc = QJsonDocument::fromJson( r->readAll() );
    if ( doc.isObject() )
    {
      QJsonObject docObj = doc.object();
      mDiskUsage = docObj.value( QStringLiteral( "disk_usage" ) ).toInt();
      mStorageLimit = docObj.value( QStringLiteral( "storage_limit" ) ).toInt();
    }
  }
  else
  {
    QString serverMsg = extractServerErrorMsg( r->readAll() );
    QString message = QStringLiteral( "Network API error: %1(): %2. %3" ).arg( QStringLiteral( "getUserInfo" ), r->errorString(), serverMsg );
    InputUtils::log( r->url().toString(), QStringLiteral( "FAILED - %1" ).arg( message ) );
    emit networkErrorOccurred( serverMsg, QStringLiteral( "Mergin API error: getUserInfo" ) );
  }

  r->deleteLater();
  emit userInfoChanged();
}


ProjectDiff MerginApi::compareProjectFiles( const QList<MerginFile> &oldServerFiles, const QList<MerginFile> &newServerFiles, const QList<MerginFile> &localFiles )
{
  ProjectDiff diff;
  QHash<QString, MerginFile> oldServerFilesMap, newServerFilesMap;

  for ( MerginFile file : newServerFiles )
  {
    newServerFilesMap.insert( file.path, file );
  }
  for ( MerginFile file : oldServerFiles )
  {
    oldServerFilesMap.insert( file.path, file );
  }

  for ( MerginFile localFile : localFiles )
  {
    QString filePath = localFile.path;
    bool hasOldServer = oldServerFilesMap.contains( localFile.path );
    bool hasNewServer = newServerFilesMap.contains( localFile.path );
    QString chkOld = oldServerFilesMap.value( localFile.path ).checksum;
    QString chkNew = newServerFilesMap.value( localFile.path ).checksum;
    QString chkLocal = localFile.checksum;

    if ( !hasOldServer && !hasNewServer )
    {
      // L-A
      diff.localAdded << filePath;
    }
    else if ( hasOldServer && !hasNewServer )
    {
      if ( chkOld == chkLocal )
      {
        // R-D
        diff.remoteDeleted << filePath;
      }
      else
      {
        // C/R-D/L-U
        diff.conflictRemoteDeletedLocalUpdated << filePath;
      }
    }
    else if ( !hasOldServer && hasNewServer )
    {
      if ( chkNew != chkLocal )
      {
        // C/R-A/L-A
        diff.conflictRemoteAddedLocalAdded << filePath;
      }
      else
      {
        // R-A/L-A
        // TODO: need to do anything?
      }
    }
    else if ( hasOldServer && hasNewServer )
    {
      // file has already existed
      if ( chkOld == chkNew )
      {
        if ( chkNew != chkLocal )
        {
          // L-U
          diff.localUpdated << filePath;
        }
        else
        {
          // no change :-)
        }
      }
      else   // v1 != v2
      {
        if ( chkNew != chkLocal && chkOld != chkLocal )
        {
          // C/R-U/L-U
          diff.conflictRemoteUpdatedLocalUpdated << filePath;
        }
        else if ( chkNew != chkLocal )  // && old == local
        {
          // R-U
          diff.remoteUpdated << filePath;
        }
        else if ( chkOld != chkLocal )  // && new == local
        {
          // R-U/L-U
          // TODO: need to do anything?
        }
        else
          Q_ASSERT( false );   // impossible - should be handled already
      }
    }

    if ( hasOldServer )
      oldServerFilesMap.remove( filePath );
    if ( hasNewServer )
      newServerFilesMap.remove( filePath );
  }

  // go through files listed on the server, but not available locally
  for ( MerginFile file : newServerFilesMap )
  {
    bool hasOldServer = oldServerFilesMap.contains( file.path );

    if ( hasOldServer )
    {
      if ( oldServerFilesMap.value( file.path ).checksum == file.checksum )
      {
        // L-D
        diff.localDeleted << file.path;
      }
      else
      {
        // C/R-U/L-D
        diff.conflictRemoteUpdatedLocalDeleted << file.path;
      }
    }
    else
    {
      // R-A
      diff.remoteAdded << file.path;
    }

    if ( hasOldServer )
      oldServerFilesMap.remove( file.path );
  }

  for ( MerginFile file : oldServerFilesMap )
  {
    // R-D/L-D
    // TODO: need to do anything?
  }

  return diff;
}


MerginProjectList MerginApi::parseListProjectsMetadata( const QByteArray &data )
{
  MerginProjectList result;

  QJsonDocument doc = QJsonDocument::fromJson( data );
  if ( doc.isArray() )
  {
    QJsonArray vArray = doc.array();

    for ( auto it = vArray.constBegin(); it != vArray.constEnd(); ++it )
    {
      QJsonObject projectMap = it->toObject();
      MerginProjectListEntry project;

      project.projectName = projectMap.value( QStringLiteral( "name" ) ).toString();
      project.projectNamespace = projectMap.value( QStringLiteral( "namespace" ) ).toString();

      QString versionStr = projectMap.value( QStringLiteral( "version" ) ).toString();
      if ( versionStr.isEmpty() )
      {
        project.version = 0;
      }
      else if ( versionStr.startsWith( "v" ) ) // cut off 'v' part from v123
      {
        versionStr = versionStr.mid( 1 );
        project.version = versionStr.toInt();
      }

      project.creator = projectMap.value( QStringLiteral( "creator" ) ).toInt();

      QJsonValue access = projectMap.value( QStringLiteral( "access" ) );
      if ( access.isObject() )
      {
        QJsonArray writers = access.toObject().value( "writers" ).toArray();
        for ( QJsonValueRef tag : writers )
        {
          project.writers.append( tag.toInt() );
        }
      }

      QDateTime updated = QDateTime::fromString( projectMap.value( QStringLiteral( "updated" ) ).toString(), Qt::ISODateWithMs ).toUTC();
      if ( !updated.isValid() )
      {
        project.serverUpdated = QDateTime::fromString( projectMap.value( QStringLiteral( "created" ) ).toString(), Qt::ISODateWithMs ).toUTC();
      }
      else
      {
        project.serverUpdated = updated;
      }

      result << project;
    }
  }
  return result;
}


QStringList MerginApi::generateChunkIdsForSize( qint64 fileSize )
{
  qreal rawNoOfChunks = qreal( fileSize ) / UPLOAD_CHUNK_SIZE;
  int noOfChunks = qCeil( rawNoOfChunks );
  QStringList chunks;
  for ( int i = 0; i < noOfChunks; i++ )
  {
    QString chunkID = QUuid::createUuid().toString( QUuid::WithoutBraces );
    chunks.append( chunkID );
  }
  return chunks;
}

QJsonArray MerginApi::prepareUploadChangesJSON( const QList<MerginFile> &files )
{
  QJsonArray jsonArray;

  for ( MerginFile file : files )
  {
    QJsonObject fileObject;
    fileObject.insert( "path", file.path );

    fileObject.insert( "checksum", file.checksum );
    fileObject.insert( "size", file.size );
    fileObject.insert( "mtime", file.mtime.toString( Qt::ISODateWithMs ) );

    QJsonArray chunksJson;
    for ( QString id : file.chunks )
    {
      chunksJson.append( id );
    }
    fileObject.insert( "chunks", chunksJson );
    jsonArray.append( fileObject );
  }
  return jsonArray;
}

void MerginApi::finishProjectSync( const QString &projectFullName, bool syncSuccessful )
{
  Q_ASSERT( mTransactionalStatus.contains( projectFullName ) );
  TransactionStatus &transaction = mTransactionalStatus[projectFullName];

  emit syncProjectStatusChanged( projectFullName, -1 );   // -1 means there's no sync going on

  if ( syncSuccessful )
  {
    // update the local metadata file
    writeData( transaction.projectMetadata, transaction.projectDir + "/" + MerginApi::sMetadataFile );

    // update info of local projects
    mLocalProjects.updateMerginLocalVersion( transaction.projectDir, transaction.version );
    mLocalProjects.updateMerginServerVersion( transaction.projectDir, transaction.version );
  }

  bool updateBeforeUpload = transaction.updateBeforeUpload;
  QString projectDir = transaction.projectDir;  // keep it before the transaction gets removed
  mTransactionalStatus.remove( projectFullName );

  if ( updateBeforeUpload )
  {
    // we're done only with the download part before the actual upload - so let's continue with upload
    QString projectNamespace, projectName;
    extractProjectName( projectFullName, projectNamespace, projectName );
    uploadProject( projectNamespace, projectName );
  }
  else
  {
    emit syncProjectFinished( projectDir, projectFullName, syncSuccessful );
  }
}

void MerginApi::copyTempFilesToProject( const QString &projectDir, const QString &projectFullName )
{
  QString tempProjectDir = getTempProjectDir( projectFullName );
  InputUtils::cpDir( tempProjectDir, projectDir );
  QDir( tempProjectDir ).removeRecursively();
}

bool MerginApi::writeData( const QByteArray &data, const QString &path )
{
  QFile file( path );
  createPathIfNotExists( path );
  if ( !file.open( QIODevice::WriteOnly ) )
  {
    return false;
  }

  file.write( data );
  file.close();

  return true;
}

void MerginApi::handleOctetStream( const QByteArray &data, const QString &projectDir, const QString &filename, bool closeFile, bool overwrite )
{
  QFile file;
  QString activeFilePath = projectDir + '/' + filename;
  file.setFileName( activeFilePath );
  createPathIfNotExists( activeFilePath );
  saveFile( data, file, closeFile, overwrite );
}

bool MerginApi::saveFile( const QByteArray &data, QFile &file, bool closeFile, bool overwrite )
{
  if ( !file.isOpen() )
  {
    if ( overwrite )
    {
      if ( !file.open( QIODevice::WriteOnly ) )
      {
        return false;
      }
    }
    else
    {
      if ( !file.open( QIODevice::Append ) )
      {
        return false;
      }
    }
  }

  file.write( data );
  if ( closeFile )
    file.close();

  return true;
}

void MerginApi::createPathIfNotExists( const QString &filePath )
{
  QDir dir;
  if ( !dir.exists( mDataDir ) )
    dir.mkpath( mDataDir );

  QFileInfo newFile( filePath );
  if ( !newFile.absoluteDir().exists() )
  {
    if ( !QDir( dir ).mkpath( newFile.absolutePath() ) )
    {
      InputUtils::log( QString( "Creating a folder failed for path: %1" ).arg( filePath ) );
    }
  }
}

void MerginApi::createEmptyFile( const QString &path )
{
  QDir dir;
  QFileInfo info( path );
  QString parentDir( info.dir().path() );
  if ( !dir.exists( parentDir ) )
    dir.mkpath( parentDir );

  QFile file( path );
  file.open( QIODevice::ReadWrite );
  file.close();
}

bool MerginApi::isInIgnore( const QFileInfo &info )
{
  return sIgnoreExtensions.contains( info.suffix() ) || sIgnoreFiles.contains( info.fileName() );
}

QByteArray MerginApi::getChecksum( const QString &filePath )
{
  QFile f( filePath );
  if ( f.open( QFile::ReadOnly ) )
  {
    QCryptographicHash hash( QCryptographicHash::Sha1 );
    QByteArray chunk = f.read( CHUNK_SIZE );
    while ( !chunk.isEmpty() )
    {
      hash.addData( chunk );
      chunk = f.read( CHUNK_SIZE );
    }
    f.close();
    return hash.result().toHex();
  }

  return QByteArray();
}

QSet<QString> MerginApi::listFiles( const QString &path )
{
  QSet<QString> files;
  QDirIterator it( path, QStringList() << QStringLiteral( "*" ), QDir::Files, QDirIterator::Subdirectories );
  while ( it.hasNext() )
  {
    it.next();
    if ( !isInIgnore( it.fileInfo() ) )
    {
      files << it.filePath().replace( path, "" );
    }
  }
  return files;
}
