#pragma once

class QThreadPool;

class BackgroundThreadPool {
public:
	static QThreadPool* instance();
};
