
#ifndef XEVD_EXPORT_H
#define XEVD_EXPORT_H

#ifdef XEVD_STATIC_DEFINE
#  define XEVD_EXPORT
#  define XEVD_NO_EXPORT
#else
#  ifndef XEVD_EXPORT
#    ifdef xevd_EXPORTS
        /* We are building this library */
#      define XEVD_EXPORT 
#    else
        /* We are using this library */
#      define XEVD_EXPORT 
#    endif
#  endif

#  ifndef XEVD_NO_EXPORT
#    define XEVD_NO_EXPORT 
#  endif
#endif

#ifndef XEVD_DEPRECATED
#  define XEVD_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef XEVD_DEPRECATED_EXPORT
#  define XEVD_DEPRECATED_EXPORT XEVD_EXPORT XEVD_DEPRECATED
#endif

#ifndef XEVD_DEPRECATED_NO_EXPORT
#  define XEVD_DEPRECATED_NO_EXPORT XEVD_NO_EXPORT XEVD_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef XEVD_NO_DEPRECATED
#    define XEVD_NO_DEPRECATED
#  endif
#endif

#endif /* XEVD_EXPORT_H */
