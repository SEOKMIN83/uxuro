typedef float	fp_t;

// keep this low to allow more blocks that share shared memory to run concurrently,
// code does not work for larger than 110, more speedup can be achieved with larger number and no shared memory used
#define NUMBER_PAR_PER_BOX 100		

// Parameterized work group size
#ifdef RD_WG_SIZE_0_0
	#define NUMBER_THREADS RD_WG_SIZE_0_0
#elif defined(RD_WG_SIZE_0)
        #define NUMBER_THREADS RD_WG_SIZE_0
#elif defined(RD_WG_SIZE)
        #define NUMBER_THREADS RD_WG_SIZE
#else
        #define NUMBER_THREADS 128
#endif

#define DOT(A,B) ((A.x)*(B.x)+(A.y)*(B.y)+(A.z)*(B.z))	// STABLE

typedef struct
{
	fp_t	x, y, z;
} THREE_VECTOR;

typedef struct
{
	fp_t	v, x, y, z;
} FOUR_VECTOR;

typedef struct nei_str
{
	// neighbor box
	int	x, y, z;
	int	number;
	long	offset;
} nei_str;

typedef struct box_str
{
	// home box
	int	x, y, z;
	int	number;
	long	offset;

	// neighbor boxes
	int	nn;
	nei_str	nei[26];
} box_str;

typedef struct par_str
{
	fp_t	alpha;
} par_str;

typedef struct dim_str
{
	// input arguments
	int	cur_arg;
	int	arch_arg;
	int	cores_arg;
	int	boxes1d_arg;

	// system memory
	long	number_boxes;
	long	box_mem;
	long	space_elem;
	long	space_mem;
	long	space_mem2;
} dim_str;
