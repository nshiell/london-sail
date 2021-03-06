/*
Copyright (C) 2014 Krisztian Olah

  email: fasza2mobile@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


#include "arrivalslogic.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QMultiMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QStringListModel>
#include <QTimer>
#include <QUrl>
#include <cmath>
#include "arrivals/arrivalsmodel.h"
#include "arrivals/arrivalsproxymodel.h"
#include "arrivals/arrivalscontainer.h"
#include "arrivals/journeyprogresscontainer.h"
#include "arrivals/stop.h"
#include "arrivals/stopsquerymodel.h"
#include "arrivals/vehicle.h"
#include "database/databasemanager.h"

ArrivalsLogic::ArrivalsLogic(DatabaseManager* dbm, QObject* parent) : QObject(parent),
                                                activeStops("StopPointState=0"),
                                                arrivalsContainer(new ArrivalsContainer()),
                                                arrivalsModel(new ArrivalsModel(arrivalsContainer,this)),
                                                arrivalsProxyModel(new ArrivalsProxyModel(this)),
                                                arrivalsTimer(new QTimer(this)),
                                                baseUrl("http://countdown.api.tfl.gov.uk/interfaces/ura/instant_V1?"),
                                                databaseManager(dbm),
                                                currentStop(new Stop(databaseManager)),
                                                displayTimer(new QTimer(this)),
                                                downloadingArrivals(false),
                                                downloadingJourneyProgress(false),
                                                downloadingListOfStops(false),
                                                downloadingStop(false),
                                                networkMngr(static_cast<QNetworkAccessManager*>(parent)),
                                                journeyProgressContainer(new JourneyProgressContainer(this)),
                                                journeyProgressTimer(new QTimer(this)),
                                                reply_arrivals(0),
                                                reply_busStop(0),
                                                reply_busStopMessage(0),
                                                reply_journeyProgress(0),
                                                reply_stations(0),
                                                reply_stops(0),
                                                stopsQueryModel(new StopsQueryModel(databaseManager))
{
    arrivalsProxyModel->setSourceModel(arrivalsModel);
    arrivalsProxyModel->sort(0);
    stopsQueryModel->showStops(Stop::Bus);

    connect(arrivalsTimer, SIGNAL(timeout()), this, SLOT(fetchArrivalsData()) );
    connect(journeyProgressTimer, SIGNAL(timeout()), this, SLOT(fetchJourneyProgress()) );
    connect(journeyProgressContainer, SIGNAL(dataChanged()), this, SLOT(onProgressDataChanged()) );
    connect(displayTimer, SIGNAL(timeout()), this, SLOT(onDisplayTimerTicked()) );
}

//private:

//clears the container holding the vehicles and their predicted eta
//the container notifies the model which notifies connected views
void ArrivalsLogic::clearArrivalsData() {
    if (arrivalsContainer) {
        arrivalsContainer->clearData();
    }
}

//clears jorneyprogress data container takes care of notifying model,views
void ArrivalsLogic::clearJourneyProgressData() {
    currentBusDirectionId = "";
    setCurrentVehicleId("");
    if(journeyProgressContainer) { journeyProgressContainer->clear(); }
}

void ArrivalsLogic::downloadStations() {
    QString path = QStandardPaths::writableLocation(QStandardPaths::DataLocation) + QString("/stations.csv");
    QFile file(path);
    if (!file.exists()) {
        QString link = "https://github.com/KrisztianOlah/london-sail/raw/devel/stations.csv";
        QUrl url(link);
        reply_stations = networkMngr->get(QNetworkRequest(url));

        connect(reply_stations,SIGNAL(finished()), this, SLOT(onStationsDownloaded()) );
    }
    else {
        bool ok = databaseManager->importStations();
        if (!ok) qDebug() << "Import Failed";
    }
}

void ArrivalsLogic::fillCurrentStopMessages(const QMap<int,QString>& map) {
    //there are 5 priorities at the moment, it may change to 6 in the near future and up to 10 in the far future
    for (int index = 0; index != 6; ++index) {
        QList<QString> listOfMessages = map.values(index);
        for (QList<QString>::const_iterator iter = listOfMessages.begin();iter < listOfMessages.end(); ++iter) {
            currentStopMessages.append(" * ");
            currentStopMessages.append(*iter);
        }
    }
    emit currentStopMessagesChanged();
}

//FIX registration num must not start with X_or contain NEW in the first five letters
//downloads the data required for bus and river bus arrivals
void ArrivalsLogic::getBusArrivalsByCode(const QString& code) {
    QString stopCode = QString("StopCode1=") + code;
    QString returnList = "&ReturnList=LineName,DestinationName,EstimatedTime,RegistrationNumber,DirectionID";
    QString request = baseUrl + stopCode +  returnList;

    QUrl url(request);
    downloadingArrivals = true;
    emit downloadStateChanged();
    reply_arrivals = networkMngr->get(QNetworkRequest(url));

    connect(reply_arrivals, SIGNAL(finished()), this, SLOT(onArrivalsDataReceived()) );
}

//downloads data required for bus journey progress
void ArrivalsLogic::getBusProgress(const QString& registrationNum) {
    QString regPart = QString("RegistrationNumber=") + registrationNum;
    QString directionIDPart = QString("&DirectionID=") + currentBusDirectionId;
    QString returnList = "&ReturnList=StopPointName,EstimatedTime";
    QString request = baseUrl + regPart + directionIDPart + returnList;
    QUrl url(request);
    downloadingJourneyProgress = true;
    emit downloadStateChanged();
    reply_journeyProgress = networkMngr->get(QNetworkRequest(url));

    connect(reply_journeyProgress, SIGNAL(finished()), this, SLOT(onBusProgressReceived()) );
}

//creates a list of QJsonDocument arrays from a QNetworkReply so long the format is Json
QList<QJsonDocument> ArrivalsLogic::makeDocument(QNetworkReply* reply) {
    QList<QJsonDocument> document;
    if (reply) {
        while (!reply->atEnd()) {
            document << QJsonDocument::fromJson(reply->readLine());
        }
        reply->deleteLater();
    }
    return document;
}

//private slots:
//calls the correct function chain for each kind of Stop to download and process arrivals data such as eta
void ArrivalsLogic::fetchArrivalsData() {
    qDebug() << "updated";
    if (currentStop) {
        switch (currentStop->getType()) {
        case Stop::None:
            return;
        case Stop::Bus:
            getBusArrivalsByCode(currentStop->getID());
            return;
        case Stop::River:
            getBusArrivalsByCode(currentStop->getID());
            return;
        }
    }

}

//calls the correct function chain for each kind of Stop to download and process journey progress
void ArrivalsLogic::fetchJourneyProgress() {
    if (currentVehicleId == "") return;
    //TODO switch on currentStop->type
    getBusProgress(currentVehicleId);
}

//gets called when bus arrivals are downloaded and ready to be processed
void ArrivalsLogic::onArrivalsDataReceived() {
    downloadingArrivals = false;
    emit downloadStateChanged();
    QList<QJsonDocument> document = makeDocument(reply_arrivals);
    QList<QJsonDocument>::iterator first = document.begin();

    //server time UTC in msec from Epoch at the time of request
    //use this to compare with expected arrival time, if device clock is not correctly set
    //the arrival times are still accurately presented to user
    if (first->array().begin() +2 >= first->array().end() ) { return; } //currentTime would be invalid
    double currentTime = (*(first->array().begin() +2)).toDouble();
    ArrivalsContainer tempContainer(arrivalsModel);
    for (QList<QJsonDocument>::iterator iter = first + 1; iter < document.end(); ++iter) {
        Vehicle bus;
        if (iter->array().begin() + 5 >= iter->array().end() ) {
            qDebug() << "";
            //TODO throw exception
            break;
        }
        bus.line = (*(iter->array().begin() + 1)).toString();
        currentBusDirectionId =  QString::number((*(iter->array().begin() + 2)).toDouble());
        bus.destination = (*(iter->array().begin() + 3)).toString();
        bus.id = (*(iter->array().begin() + 4)).toString();//registration number
        double delta = (*(iter->array().begin() + 5)).toDouble() - currentTime;
        double inSec = delta / 1000;
        double inMins = inSec / 60;
        //round to whole numbers
        bus.eta = std::round(inMins);
        tempContainer.add(bus);
    }
    if (arrivalsContainer) {
        arrivalsContainer->replace(tempContainer);
    }
}

//gets called when bus progress data is downloaded and redy to be processed
void ArrivalsLogic::onBusProgressReceived() {
    downloadingJourneyProgress = false;
    emit downloadStateChanged();
    QList<QJsonDocument> document = makeDocument(reply_journeyProgress);
    if (document.begin() == document.end() ||
            document.begin()->array().begin() + 2 >= document.begin()->array().end()) {
        return;
    }//nothing to do
    double serverTime = (*(document.begin()->array().begin() +2)).toDouble();
    //BUG check why list might be empty, server or client error
    QList<QPair<QString, double>> list;
    if (document.begin() + 1 >= document.end()) {
    }
    for (QList<QJsonDocument>::iterator iter = document.begin() + 1; iter < document.end(); ++iter) {
        if (iter->array().begin() +2 >= iter->array().end()) {
            //TODO throw
            break;
        }
        QPair<QString, double> pair;
        pair.first = (*(iter->array().begin() +1)).toString();
        pair.second = (*(iter->array().begin() +2)).toDouble();
        list.append(pair);
    }
    if (journeyProgressContainer) {
        journeyProgressContainer->setTime(serverTime);
        journeyProgressContainer->refreshData(list);
    }
}

//gets called when bus stop data is downloaded and ready to be processed
void ArrivalsLogic::onBusStopDataReceived() {
    downloadingStop = false;
    emit downloadStateChanged();
    QList<QJsonDocument> document = makeDocument(reply_busStop);
    //only want the second array as the first one is the version array and there are only 2 arrays

    if (document.begin() + 1 >= document.end()) { return; } //there is nothing to do
    QList<QJsonDocument>::iterator dataArray = document.begin() + 1;
    if (currentStop) {
        if (dataArray->isArray() ) {
            if (dataArray->array().begin() + 6 >= dataArray->array().end()) { return; } //TODO throw
            //id is set in ArrivalsLogic::getBusStopByCode(const QString&)
            currentStop->setName( (*(dataArray->array().begin() + 1)).toString() );
            currentStop->setTowards( (*(dataArray->array().begin() + 3)).toString() );
            currentStop->setStopPointIndicator( (*(dataArray->array().begin() + 4)).toString() );
            currentStop->setLatitude( (*(dataArray->array().begin() + 5)).toDouble() );
            currentStop->setLongitude( (*(dataArray->array().begin() + 6)).toDouble() );
            if ( (*(dataArray->array().begin() + 2)).toString() == QString("SLRS") ) {
                currentStop->setType(Stop::River);
            }
            else { currentStop->setType(Stop::Bus); }

            currentStop->updated();
            emit stopDataChanged();
        }
        else qDebug() << "Invalid QJsonArray";
    }
}

//when getBusStopMessage(const QString&) download finishes
void ArrivalsLogic::onBusStopMessageReceived() {
    QList<QJsonDocument> document = makeDocument(reply_busStopMessage);
    if (document.empty()) { return; } //nothing to do

    QJsonArray versionArray = document.begin()->array();
    if (versionArray.begin() + 2 < versionArray.end() ) {
        double serverTime = (*(versionArray.begin() + 2 )).toDouble();
        QMultiMap<int,QString> messages;
        for (QList<QJsonDocument>::const_iterator iter = document.begin() + 1; iter < document.end(); ++iter) {
            if (iter->array().begin() + 4 < iter->array().end()) {
                int priority = (*(iter->array().begin() + 1)).toDouble();
                QString text = (*(iter->array().begin() + 2)).toString();
                double startTime = (*(iter->array().begin() + 3)).toDouble();
                double expireTime = (*(iter->array().begin() + 4)).toDouble();
                if (startTime <= serverTime && expireTime >= serverTime) {
                    messages.insert(priority, text);
                }
            }
            else {
                qDebug() << "The array doesn't contain 4 elements.";
                break;
            }
        }
        fillCurrentStopMessages(messages);
    }
    else return;
}

void ArrivalsLogic::onDisplayTimerTicked() {
    emit displayTimerTicked();
}

//gets called when the list of bus stops are downloaded by getBusStopsByName(name)
void ArrivalsLogic::onListOfBusStopsReceived() {
    downloadingListOfStops = false;
    emit downloadStateChanged();
    QList<QJsonDocument> document = makeDocument(reply_stops);
    QString stopPointType;
    //skip version array
    for (QList<QJsonDocument>::const_iterator iter = document.begin() + 1;iter < document.end();++iter) {
        if (iter->array().begin() + 7 >= iter->array().end()) {
            break;
        } //TODO throw
        Stop stop(databaseManager);
        stop.setName((*(iter->array().begin() + 1)).toString());
        stop.setID((*(iter->array().begin() + 2)).toString());
        stop.setTowards((*(iter->array().begin() + 4)).toString());
        stop.setStopPointIndicator((*(iter->array().begin() + 5)).toString());
        stop.setLatitude((*(iter->array().begin() + 6)).toDouble());
        stop.setLongitude((*(iter->array().begin() + 7)).toDouble());
        stopPointType = (*(iter->array().begin() + 3)).toString();
        if ( stopPointType == QString("SLRS")) {
            stop.setType(Stop::River);
        }
        else {
            stop.setType(Stop::Bus);
        }
        //The meaning of these codes are documented in the Bus arrivals API documentation
        //only display sstops with these codes
        if (stopPointType == "STBR" || stopPointType == "STBC" || stopPointType == "SRVA" ||
            stopPointType == "STZZ" || stopPointType == "STBN" || stopPointType == "SLRS" ||
            stopPointType == "STBS" || stopPointType == "STSS") {

            //to prevent a bug when server returns a stop where code isNull() ie: Hammersmith Bus Station
            if (!(*(iter->array().begin() + 2)).isNull()) { stop.addToDb(); }
        }
    }
    if (stopsQueryModel) {
        stopsQueryModel->showStops(Stop::Bus);
    }
}

//signals to gui that there is a new next stop
void ArrivalsLogic::onProgressDataChanged() {
    qDebug() << "NewStop: " << getNextStop();
    emit nextStopChanged();
}

void ArrivalsLogic::onStationsDownloaded() {
    int httpCode = reply_stations->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    qDebug() << "HTTP response" << httpCode;

    if (reply_stations->error() != QNetworkReply::NoError) {
        qDebug() << "HTTP returned error:" << httpCode;
    }

    if (httpCode > 300 && httpCode < 400) {
        QVariant redirectUrl = reply_stations->attribute(QNetworkRequest::RedirectionTargetAttribute);
        reply_stations->deleteLater();
        QUrl url = reply_stations->url().resolved(redirectUrl.toUrl());
        qDebug() << "redirecting to" << url;
        reply_stations = networkMngr->get(QNetworkRequest(url));

        connect(reply_stations,SIGNAL(finished()),this, SLOT(onStationsDownloaded()) );
    }
    else if (httpCode < 300 && httpCode >= 200) {
        QString path = QStandardPaths::writableLocation(QStandardPaths::DataLocation);
        QDir dir(path);
        if (!dir.exists()) {
            dir.mkpath(path);
        }

        path += QString("/stations.csv");
        QFile file(path);

        if (!file.open(QIODevice::WriteOnly)) {
            qDebug() << "Error: Couldn't open stations.csv for writing";
        }
        else {
            file.write(reply_stations->readAll());
            file.close();
            bool ok = databaseManager->importStations();
            if (!ok) qDebug() << "Import Failed";
        }
        reply_stations->deleteLater();
    }
}

//public slots:
//called by BusStopPage onDestruction()
void ArrivalsLogic::clearCurrentStop() {
    currentStop->clear();
    currentStopMessages.clear();
}

//makes stop a favorite or removes it from favorites depending on the second arg
bool ArrivalsLogic::favorStop(const QString& code, bool b) {
    if (!databaseManager) { return false; }
    bool ok = false;
    if (b) {
        ok = databaseManager->makeFavorite(code);
        stopsQueryModel->showStops(Stop::Bus);
        return ok;
    }
    else {
        ok = databaseManager->unFavorite(code);
        stopsQueryModel->showStops(Stop::Bus);
        return ok;
    }
}

ArrivalsProxyModel* ArrivalsLogic::getArrivalsModel() { return arrivalsProxyModel; }

//downloads bus stop data for a bus stop  with a given code
void ArrivalsLogic::getBusStopByCode(const QString& code) {
    currentStop->setID(code);

    QString stopcode = QString("StopCode1=") + code;
    QString returnList = "&ReturnList=StopPointName,Towards,StopPointIndicator,StopPointType,Latitude,Longitude";
    QString request = baseUrl + stopcode + returnList;
    QUrl url(request);
    downloadingStop = true;
    emit downloadStateChanged();
    reply_busStop = networkMngr->get(QNetworkRequest(url));

    connect(reply_busStop, SIGNAL(finished()), this, SLOT(onBusStopDataReceived()) );
}

void ArrivalsLogic::getBusStopMessage(const QString& code) {
    QString stopCode = QString("StopCode1=") + code;
    QString returnList = "&ReturnList=MessagePriority,MessageText,StartTime,ExpireTime";
    QString request = baseUrl + stopCode + returnList;
    QUrl url(request);

    reply_busStopMessage = networkMngr->get(QNetworkRequest(url));

    connect(reply_busStopMessage, SIGNAL(finished()), this, SLOT(onBusStopMessageReceived()) );
}

//downloads a list of stops that bear the same name
void ArrivalsLogic::getBusStopsByName(const QString& name) {
    QString stopPointName = QString("StopPointName=") + name;
    QString returnList = "&ReturnList=StopPointName,StopCode1,Towards,StopPointIndicator,StopPointType,Latitude,Longitude";
    QString request = baseUrl + stopPointName + returnList;
    QUrl url = request;
    downloadingListOfStops = true;
    emit downloadStateChanged();
    reply_stops = networkMngr->get(QNetworkRequest(url) );
    connect(reply_stops, SIGNAL(finished()), this, SLOT(onListOfBusStopsReceived()) );
}

QString ArrivalsLogic::getCurrentDestination() const { return currentDestination;}

Stop* ArrivalsLogic::getCurrentStop() { return currentStop; }

QString ArrivalsLogic::getCurrentStopMessages() const {
    return currentStopMessages;
}

QString ArrivalsLogic::getCurrentVehicleLine() const { return currentVehicleLine; }

double ArrivalsLogic::getTimerProgress_arrivals() const {
    double interval = arrivalsTimer->interval();
    double remaining = arrivalsTimer->remainingTime();
    return (interval - remaining) / interval * 100;
}

double ArrivalsLogic::getTimerProgress_journeyProgress() const {
    double interval = journeyProgressTimer->interval();
    double remaining = journeyProgressTimer->remainingTime();
    return (interval - remaining) / interval * 100;
}

bool ArrivalsLogic::isDownloadingArrivals() const { return downloadingArrivals; }

bool ArrivalsLogic::isDownloadingJourneyProgress() const { return downloadingJourneyProgress; }

bool ArrivalsLogic::isDownloadingListOfStops() const { return downloadingListOfStops; }

bool ArrivalsLogic::isDownloadingStop() const { return downloadingStop; }

ArrivalsProxyModel* ArrivalsLogic::getJourneyProgressModel() { return journeyProgressContainer->getModel(); }

QString ArrivalsLogic::getNextStop() { return journeyProgressContainer->getNextStop(); }

StopsQueryModel* ArrivalsLogic::getStopsQueryModel() { return stopsQueryModel; }

bool ArrivalsLogic::isStopFavorite(const QString& code) { return databaseManager->isFavorite(code); }

void ArrivalsLogic::refreshArrivalsModel() {
    arrivalsModel->refresh();
}

void ArrivalsLogic::setCurrentDestination(const QString& destination) { currentDestination = destination; }

void ArrivalsLogic::setCurrentVehicleId(const QString& id) { currentVehicleId = id;}

void ArrivalsLogic::setCurrentVehicleLine(const QString& line) { currentVehicleLine = line; }

//set stopsQueryModel to show one of the preset queries, type = 0 will return all stops in db
void ArrivalsLogic::setStopsQueryModel(int type) {
    qDebug() << "setStopsQueryModel called with" << type;
    if (!databaseManager->areTubeStationsInDB()) {
        qDebug() << "Calling downloadStations()";
        downloadStations();
    }
    else qDebug() << "There are already tubestations in db";
    stopsQueryModel->showStops(type);
}

//starts timer to periodically download arrivals data
//time interval might be different for each kind of stops
void ArrivalsLogic::startArrivalsUpdate() {
    fetchArrivalsData();
    arrivalsTimer->start(30000);//30 sec
    displayTimer->start(16);
}

//starts timer to periodically download journey progress data
//time interval might be different for each kind of stops
void ArrivalsLogic::startJourneyProgressUpdate() {
    qDebug() << "***startJourneyProgressUpdate() ***";
    fetchJourneyProgress();
    journeyProgressTimer->start(30000);//30 sec
    displayTimer->start(16);
}

//stops timer to download arrivals data
void ArrivalsLogic::stopArrivalsUpdate() {
    qDebug() << "updating stopped.";
    displayTimer->stop();
    arrivalsTimer->stop();
    clearArrivalsData();
}

//stops timer to download journey progress data
void ArrivalsLogic::stopJourneyProgressUpdate() {
    displayTimer->stop();
    journeyProgressTimer->stop();
    clearJourneyProgressData();
}

