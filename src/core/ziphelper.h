/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef ZIPHELPER_H
#define ZIPHELPER_H

#include <QString>
#include <QDir>
#include <QDirIterator>
#include <QVector>

#include "zip/src/zip.h"

class ZipHelper
{
public:
	ZipHelper() = default;

	static void packageFiles(const QString &outputPath, const QString &pathToFiles) {
		// Get all the files and directories in the temporary directory
		QDir temporaryDirectory(pathToFiles);
		QDirIterator projectDirIterator(
			pathToFiles,
			QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs | QDir::Hidden,
			QDirIterator::Subdirectories
		);

		// Generate a listing for all the entries in the directory
		QVector<QString> fileNames;
		while (projectDirIterator.hasNext()) fileNames.push_back(projectDirIterator.next());

		// Open a basic zip file for writing, the 'w' parameter ensures we always write a new copy and not append
		struct zip_t *zip = zip_open(outputPath.toStdString().c_str(), ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');

		for (int i = 0; i < fileNames.count(); ++i) {
			QFileInfo fInfo(fileNames[i]);
			// We need to pay special attention to directories since we need to create empty ones as well
			if (fInfo.isDir()) {
				// Will only create the directory if / is appended
				zip_entry_open(zip, QString(temporaryDirectory.relativeFilePath(fileNames[i]) + "/").toStdString().c_str());
				zip_entry_fwrite(zip, fileNames[i].toStdString().c_str());
			}
			// If we are writing a regular file, just write as is to the archive
			else {
				zip_entry_open(zip, temporaryDirectory.relativeFilePath(fileNames[i]).toStdString().c_str());
				zip_entry_fwrite(zip, fileNames[i].toStdString().c_str());
			}

			// Close each entry handle after a successful write
			zip_entry_close(zip);
		}

		// Close the archive after all writes are complete
		zip_close(zip);
	}

	static void extractFiles(const QString &archivePath, const QString &directoryToExtractTo) {
		zip_extract(archivePath.toStdString().c_str(),
					directoryToExtractTo.toStdString().c_str(),
					nullptr, nullptr);
	}
};

#endif // ZIPHELPER_H