#ifndef GT_EPOLL_DEFINITION_H_
#define GT_EPOLL_DEFINITION_H_


typedef void(*gtepoll_callback)(void* data, unsigned int datalen);

enum GTEPOLL_ERRORCODE{
	GTERROR_FAILED = -1,
	GTERROR_SUCCESS = 0
};

enum GTEPOLL_CALLBACK_TYPE{
	GTEPOLL_READ = 0,
	GTEPOLL_WRITE = 1,
    GTEPOLL_CONN = 2
};

#endif
