extern void	co_init		P((unsigned int, int));
extern uindex	co_new		P((object*, string*, Int, frame*, int));
extern Int	co_del		P((object*, unsigned int));
extern array   *co_list		P((dataspace*, object*));
extern void	co_call		P((frame*));
extern void	co_info    	P((uindex*, uindex*));
extern bool	co_ready	P((void));
extern long	co_swaprate1 	P((void));
extern long	co_swaprate5 	P((void));
extern bool	co_dump		P((int));
extern void	co_restore	P((int, Uint));
