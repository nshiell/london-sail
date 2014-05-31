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

#include "streetmodel.h"

StreetModel::StreetModel(QList<Street>* s,QObject* parent) : QAbstractListModel(parent),
                                                             streets(s)
{
}

QVariant StreetModel::data(const QModelIndex& index, int role) const {
    switch (role) {
    case NameRole:
        return streets->at(index.row()).name;
    case ClosureRole:
        return streets->at(index.row()).closure;
    case DirectionsRole:
        return streets->at(index.row()).directions;
    }
    return QVariant();
}

QHash<int,QByteArray> StreetModel::roleNames() const {
    QHash<int,QByteArray> roles;
    roles[NameRole] = "nameData";
    roles[ClosureRole] = "closureData";
    roles[DirectionsRole] = "directionsData";
    return roles;
}

int StreetModel::rowCount(const QModelIndex& /*parent*/) const {
    if (streets) { return streets->size(); }
    else return 0;
}

//public slots:
int StreetModel::count() { return rowCount(); }