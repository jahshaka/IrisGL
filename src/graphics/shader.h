/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef SHADERPROGRAM_H
#define SHADERPROGRAM_H

#include "../irisglfwd.h"
#include <QMap>
#include <QSet>
#include <QString>

namespace iris
{

// Shader source text carrier. The GL program/uniform half died with the legacy
// renderer at step 14; what remains is the document data: the shadergraph
// module and CustomMaterial (.effect files) pass GLSL text around through this
// type, and the flags feed material serialization.
class Shader
{
    friend class Material;

public:
    static ShaderPtr load(QString vertexShaderFile, QString fragmentShaderFile);
    static ShaderPtr create(QString vertexShader, QString fragmentShader);
	static ShaderPtr create();

    ~Shader();

	void setVertexShader(QString vertexShader);
	void setFragmentShader(QString fragmentShader);

	QString getVertexShader() const { return vertexShader; }
	QString getFragmentShader() const { return fragmentShader; }

	bool isFlagEnabled(QString flag);
	void enableFlag(QString flag);
	void disableFlag(QString flag);

private:
	long shaderId;

	Shader();

    static long generateNodeId();
    static long nextId;

protected:
	QString vertexShader, fragmentShader;
	QSet<QString> flags;
};

}

#endif // SHADERPROGRAM_H
