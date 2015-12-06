/*********************************************************************************
NixNote - An open-source client for the Evernote service.
Copyright (C) 2015 Randy Baumgarte

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
***********************************************************************************/



#include "cmdlinetool.h"
#include "global.h"
#include <iostream>
#include <unistd.h>
#include "html/enmlformatter.h"
#include "utilities/crossmemorymapper.h"
#include "filters/filtercriteria.h"
#include "filters/filterengine.h"
#include "sql/notebooktable.h"
#include "sql/tagtable.h"
#include "sql/notetable.h"
#include "utilities/nuuid.h"
#include "email/smtpclient.h"
#include "utilities/mimereference.h"

extern Global global;

CmdLineTool::CmdLineTool(QObject *parent) :
    QObject(parent)
{
}


int CmdLineTool::run(StartupConfig config) {
    // Force info level messages only
    QsLogging::Logger& logger = QsLogging::Logger::instance();
    logger.setLoggingLevel(QsLogging::InfoLevel);

    QString errmsg(tr("Unable to attach to shared memory segment.  Is the other NixNote running?\n"));
    if (config.sync()) {
        if (!global.sharedMemory->attach()) {
            std::cout << errmsg.toStdString();
            return 16;
        }
        global.sharedMemory->write(QString("SNCHRONIZE"));
        global.sharedMemory->detach();
        return 1;
    }
    if (config.shutdown()) {
        if (!global.sharedMemory->attach()) {
            std::cout << errmsg.toStdString();
            return 16;
        }
        global.sharedMemory->write(QString("IMMEDIATE_SHUTDOWN"));
        global.sharedMemory->detach();
        return 1;
    }
    if (config.show()) {
        if (!global.sharedMemory->attach()) {
            std::cout << errmsg.toStdString();
            return 16;
        }
        global.sharedMemory->write(QString("SHOW_WINDOW"));
        global.sharedMemory->detach();
        return 1;
    }


    if (config.addNote()) {
        return addNote(config);
    }

    if (config.query()) {
        return queryNotes(config);
    }
    if (config.deleteNote()) {
        return deleteNote(config);
    }
    if (config.emailNote()) {
        return emailNote(config);
    }
    return 0;
}




int CmdLineTool::emailNote(StartupConfig config) {
    // Look to see if another NixNote is running.  If so, then we
    // expect a response if the note was delete.  Otherwise, we
    // do it ourself.
    bool useCrossMemory = true;
    global.sharedMemory->unlock();
    global.sharedMemory->detach();
    if (!global.sharedMemory->attach()) {
        useCrossMemory = false;
    }
    if (useCrossMemory) {
        global.sharedMemory->write("EMAIL_NOTE:" + config.email->wrap());
    } else {
        global.db = new DatabaseConnection("nixnote");  // Startup the database
        return config.email->sendEmail();
    }
    return 0;
}



int CmdLineTool::deleteNote(StartupConfig config) {
    bool useCrossMemory = true;

    if (config.delNote->verifyDelete) {
        std::string verify;
        std::cout << QString(tr("Type DELETE to very: ")).toStdString();
        std::cin >> verify;
        QString qVerify = QString::fromStdString(verify);
        if (qVerify.toLower() != "delete")
            return 16;
    }

    // Look to see if another NixNote is running.  If so, then we
    // expect a response if the note was delete.  Otherwise, we
    // do it ourself.
    global.sharedMemory->unlock();
    global.sharedMemory->detach();
    if (!global.sharedMemory->attach()) {
        useCrossMemory = false;
    }
    if (useCrossMemory) {
        global.sharedMemory->write("DELETE_NOTE:" + QString::number(config.delNote->lid));
    } else {
        global.db = new DatabaseConnection("nixnote");  // Startup the database
        NoteTable noteTable(global.db);
        noteTable.deleteNote(config.delNote->lid,true);
    }
    return 0;
}



int CmdLineTool::queryNotes(StartupConfig config) {
    bool expectResponse = true;

    // Look to see if another NixNote is running.  If so, then we
    // expect a response of the LID created.  First we detach it so
    // we are not talking to ourselves.
    global.sharedMemory->unlock();
    global.sharedMemory->detach();
    if (!global.sharedMemory->attach()) {
        expectResponse = false;
    }
    if (expectResponse) {
        NUuid uuid;
        config.queryNotes->returnUuid = uuid.create();
        QString queryString = config.queryNotes->wrap();
        global.sharedMemory->write("CMDLINE_QUERY:" + queryString);

        int cnt = 0;
        int maxWait=10;
        QString tmpFile = global.fileManager.getTmpDirPath()+config.queryNotes->returnUuid+".txt";
        QFile responseFile(tmpFile);
        while (!responseFile.exists() && cnt<maxWait) {
            sleep(1);
            cnt++;
        }
        bool goodResponse = false;
        if (responseFile.exists()) {
            if (responseFile.open(QIODevice::ReadOnly)) {
               QTextStream in(&responseFile);
               while (!in.atEnd()) {
                  QString line = in.readLine();
                  std::cout << line.toStdString() << std::endl;
               }
               responseFile.close();
               goodResponse = true;
            }
        }
        if (!goodResponse)
            std::cout << QString(tr("No resospnse received from NixNote.")).toStdString() << std::endl;
    } else {
        // The other NixNote isn't found, so we do the query ourself
        global.db = new DatabaseConnection("nixnote");  // Startup the database
        FilterCriteria *filter = new FilterCriteria();
        global.filterCriteria.append(filter);
        global.filterPosition = 0;
        FilterEngine engine;
        filter->setSearchString(config.queryNotes->query);
        QList<qint32> lids;
        engine.filter(filter, &lids);
        config.queryNotes->write(lids, "");
    }
    return 0;
}



int CmdLineTool::addNote(StartupConfig config) {

#ifdef Q_OS_WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    // If we are reding stdin
    if (config.newNote->content == "")  {
        QByteArray content;

        while(!std::cin.eof()) {
            char arr[1024];
            std::cin.read(arr,sizeof(arr));
            int s = std::cin.gcount();
            content.append(arr,s);
            content.append("<br>");
            content.replace("\n","<br>");
        }
        config.newNote->content = QString::fromAscii(content);
    }

    if (!config.newNote->content.contains("<body")) {
        config.newNote->content.prepend("<body>");
    }
    if (!config.newNote->content.contains("</body")) {
        config.newNote->content.append("</body>");
    }


    EnmlFormatter formatter;
    formatter.setHtml(config.newNote->content);
    config.newNote->content = formatter.rebuildNoteEnml();

    bool expectResponse = true;

    // Look to see if another NixNote is running.  If so, then we
    // expect a response of the LID created.  First we detach it so
    // we are not talking to ourselves.
    global.sharedMemory->unlock();
    global.sharedMemory->detach();
    if (!global.sharedMemory->attach()) {
        expectResponse = false;
    } else {
        global.sharedMemory->detach();
    }
    if (expectResponse) {
        NUuid uuid;
        QString id = uuid.create();

        // Setup cross memory for response
        CrossMemoryMapper crossMemory(id);
        if (!crossMemory.allocate(512))
            expectResponse = false;

        // Write out the segment
        config.newNote->write(id);
        // Start checking for the response
        int cnt = 0;
        qint32 newLid = -1;
        int maxWait = 5;
        while (expectResponse && cnt<maxWait) {
            QByteArray data = crossMemory.read();
            if (!data.startsWith('\0')) {
                expectResponse = false;
                newLid = data.toInt();
            } else {
                sleep(1);
            }
            cnt++;
        }
        if (newLid > 0) {
            std::cout << newLid << QString(tr(" has been created.\n")).toStdString();
            return newLid;
        } else {
            std::cout << QString(tr("No response from NixNote.  Please verify that the note was created.\n")).toStdString();
        }
    } else {
       // Another NN isn't found, so we do this ourself
        global.db = new DatabaseConnection("nixnote");  // Startup the database
        NUuid uuid;
        Note newNote;
        newNote.content = config.newNote->content;
        newNote.active = true;
        newNote.created = QDateTime::currentMSecsSinceEpoch();
        newNote.guid = uuid.create();
        if (config.newNote->title != "")
            newNote.title = config.newNote->title;
        else
            newNote.title = tr("Untitled Note");

        // Process tags
        newNote.tagGuids = QList<Guid>();
        newNote.tagNames=QStringList();
        for (int i=0; i<config.newNote->tags.size(); i++) {
            QString tagName = config.newNote->tags[i];
            TagTable tagTable(global.db);
            qint32 tagLid = tagTable.findByName(tagName, 0);
            QString tagGuid;

            // Do we need to add the tag?
            if (tagLid == 0) {
                Tag tag;
                tag.name = tagName;
                NUuid uuid;
                tagGuid = uuid.create();
                tag.guid = tagGuid;
                tagTable.add(0, tag, true, 0);
            } else {
                tagTable.getGuid(tagGuid, tagLid);
            }
            newNote.tagNames->append(tagName);
            newNote.tagNames->append(tagGuid);
        }

        // Process the notebook
        if (config.newNote->notebook != "") {
            QString notebookName = config.newNote->notebook;
            NotebookTable notebookTable(global.db);
            qint32 lid = notebookTable.findByName(notebookName);
            QString notebookGuid;

            // Do we need to add the notebook?
            if (lid == 0) {
                Notebook book;
                book.name = notebookName;
                NUuid uuid;
                QString newGuid = uuid.create();
                book.guid = newGuid;
                notebookGuid = newGuid;
                lid = notebookTable.add(0, book, true, false);
            } else {
                notebookTable.getGuid(notebookGuid, lid);
            }
            newNote.notebookGuid = notebookGuid;
        } else {
            NotebookTable notebookTable(global.db);
            newNote.notebookGuid = notebookTable.getDefaultNotebookGuid();
        }

        // Do the dates
        if (config.newNote->created != "") {
            QString dateString = config.newNote->created;
            QDateTime date = QDateTime::fromString(dateString, "yyyy-MM-ddTHH:mm:ss.zzzZ");
            newNote.created = date.toMSecsSinceEpoch();
        }
        if (config.newNote->updated != "") {
            QString dateString = config.newNote->updated;
            QDateTime date = QDateTime::fromString(dateString, "yyyy-MM-ddTHH:mm:ss.zzzZ");
            newNote.updated = date.toMSecsSinceEpoch();
        }

        NoteTable noteTable(global.db);
        qint32 newLid = noteTable.addStub(newNote.guid);
        // Do the attachments
        for (int i=0; i<config.newNote->attachments.size(); i++) {
            QString filename = config.newNote->attachments[i];
            QFile file(filename);
            if (file.exists()) {

                file.open(QIODevice::ReadOnly);
                QByteArray ba = file.readAll();
                file.close();

                MimeReference mimeRef;
                QString extension = filename;
                int endPos = filename.lastIndexOf(".");
                if (endPos != -1)
                    extension = extension.mid(endPos);
                QString mime =  mimeRef.getMimeFromExtension(extension);
                Resource newRes;
                bool attachment = true;
                if (mime == "application/pdf" || mime.startsWith("image/"))
                    attachment = false;
                config.newNote->createResource(newRes, 0, ba, mime, attachment, QFileInfo(filename).fileName(), newLid);
                QByteArray hash;
                if (newRes.data.isSet()) {
                    Data d = newRes.data;
                    if (d.bodyHash.isSet())
                        hash = d.bodyHash;
                }
                if (!newNote.resources.isSet()) {
                    newNote.resources = QList<Resource>();
                }
                QString mediaString = "<en-media hash=\""+hash.toHex()+"\"; type=\""+mime+"\"/>";
                if (newNote.content->contains(config.newNote->attachmentDelimiter)) {
                     //newNote.content = newNote.content->replace(config.newNote->attachmentDelimiter,mediaString);
                     newNote.content = newNote.content->replace(newNote.content->indexOf(config.newNote->attachmentDelimiter),
                                                         config.newNote->attachmentDelimiter.size(), mediaString);
                } else {
                    newNote.content = newNote.content->replace("</en-note>","<br>"+mediaString+"</en-note>");
                }
                newNote.resources->append(newRes);
            }
        }
        noteTable.expunge(newLid);
        noteTable.add(newLid,newNote,true);
        std::cout << newLid << QString(tr(" has been created.\n")).toStdString();
        return newLid;
    }
    return 0;
}
