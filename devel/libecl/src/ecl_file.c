#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fortio.h>
#include <ecl_kw.h>
#include <ecl_file.h>
#include <errno.h>
#include <hash.h>
#include <util.h>
#include <time.h>
#include <vector.h>
#include <int_vector.h>
#include <stringlist.h>


/**
   This file implements functionality to load an entire ECLIPSE file
   in ecl_kw format. In addition to loading a complete file it can
   also load a section, by stopping when it meets a certain
   keyword. This latter functionality is suitable for loading parts
   (i.e. one report step) of unified files.

   The ecl_file struct is quite simply a vector of ecl_kw instances,
   it has no knowledge of report steps and such, and does not know
   whether it has been built from a complete file, or only from a part
   of a unified file.
*/





#define ECL_FILE_ID 776107


struct ecl_file_struct {
  int         	    __id;
  char        	  * src_file;  	  /* The name of the file currently loaded - as returned from fortio. */
  vector_type 	  * kw_list;   	  /* This is a vector of ecl_kw instances corresponding to the content of the file. */
  hash_type   	  * kw_index;  	  /* A hash table with integer vectors of indices - see comment below. */
  stringlist_type * distinct_kw;  /* A stringlist of the keywords occuring in the file - each string occurs ONLY ONCE. */
};


/*
  This illustrates the indexing. The ecl_file instance contains in
  total 7 ecl_kw instances, the global index [0...6] is the internal
  way to access the various keywords. The kw_index is a hash table
  with entries 'SEQHDR', 'MINISTEP' and 'PARAMS'. Each entry in the
  hash table is an integer vector which again contains the internal
  index of the various occurences:
  
   ------------------
   SEQHDR            \
   MINISTEP  0        |     
   PARAMS    .....    |
   MINISTEP  1        |
   PARAMS    .....    |
   MINISTEP  2        |
   PARAMS    .....   /
   ------------------

   kw_index    = {"SEQHDR": [0], "MINISTEP": [1,3,5], "PARAMS": [2,4,6]}    <== This is hash table.
   kw_list     = [SEQHDR , MINISTEP , PARAMS , MINISTEP , PARAMS , MINISTEP , PARAMS]
   distinct_kw = [SEQHDR , MINISTEP , PARAMS]
   
*/



static bool ecl_file_assert_type(const ecl_file_type * ecl_file) {
  if (ecl_file->__id != ECL_FILE_ID) {
    util_abort("%s: run_time cast failed - aborting \n",__func__);
    return false;
  } else
    return true;
}


static ecl_file_type * ecl_file_safe_cast( void * arg) {
  ecl_file_type * ecl_file = (ecl_file_type * ) arg;
  if (ecl_file_assert_type(ecl_file))
    return ecl_file;
  else
    return NULL;
}
  



static ecl_file_type * ecl_file_alloc_empty( ) {
  ecl_file_type * ecl_file = util_malloc( sizeof * ecl_file , __func__);
  ecl_file->kw_list  	= vector_alloc_new();
  ecl_file->__id     	= ECL_FILE_ID;
  ecl_file->kw_index 	= hash_alloc();
  ecl_file->src_file 	= NULL;
  ecl_file->distinct_kw = stringlist_alloc_new();
  return ecl_file;
}


/**
   This function iterates over the kw_list vector and builds the
   internal index fields 'kw_index' and 'distinct_kw'. This function
   must be called everytime the content of the kw_list vector is
   modified (otherwise the ecl_file instance will be in an
   inconsistent state).
*/

static void ecl_file_make_index( ecl_file_type * ecl_file ) {
  stringlist_clear( ecl_file->distinct_kw );
  hash_clear( ecl_file->kw_index );
  {
    int i;
    for (i=0; i < vector_get_size( ecl_file->kw_list ); i++) {
      const ecl_kw_type * ecl_kw = vector_iget_const( ecl_file->kw_list , i);
      char              * header = ecl_kw_alloc_strip_header( ecl_kw );
      
      if (! hash_has_key( ecl_file->kw_index , header )) {
	hash_insert_hash_owned_ref( ecl_file->kw_index , header , int_vector_alloc(0 , -1) , int_vector_free__);
	stringlist_append_copy( ecl_file->distinct_kw , header);
      }
      
      {
	int_vector_type * index_vector = hash_get( ecl_file->kw_index , header);
	int_vector_append( index_vector , i);
      }
      free(header);
    }
  }
}




/* 
   This function will allocate and read a ecl_file instance from an
   open fortio instance. If stop_kw != NULL the function will return
   __THE_SECOND_TIME__ stop_kw is encountered, leaving the fortio
   pointer looking at the stop_kw keyword. If stop_kw == NULL the
   function will read the complete file. The stop_kw parameter is used
   to support partial reading of unified files (RFT/SUMMARY/RESTART).

   If no keywords are read in the function will return NULL (and not
   an empty ecl_file skeleton).
*/


static ecl_file_type * ecl_file_fread_alloc_fortio(fortio_type * fortio , const char * stop_kw) {
  bool first_kw            = true;
  int stop_count           = 0;
  ecl_file_type * ecl_file = ecl_file_alloc_empty();
  ecl_kw_type   * ecl_kw;

  ecl_file->src_file = util_alloc_string_copy( fortio_filename_ref( fortio ) );
  do {
    ecl_kw = ecl_kw_fread_alloc( fortio );
    if (ecl_kw != NULL) {
      if (stop_kw != NULL) {
	bool eq = ecl_kw_header_eq( ecl_kw , stop_kw);
	
	if (first_kw) 
	  if (!eq)
	    util_abort("%s: expected to find:%s as first keyword - aborting \n",__func__ , stop_kw);
	
	if (eq)
	  stop_count++;
	
	if (stop_count == 2) { /* Two strikes and you are out ... */
	  ecl_kw_rewind(ecl_kw , fortio);
	  ecl_kw_free( ecl_kw );
	  ecl_kw = NULL;
	}
      }
    }
    
    /* 
       Must have a new check on NULL - because the ecl_kw instance can
       be freed and set to to NULL in the preceeding block. 
    */
    if (ecl_kw != NULL) 
      vector_append_owned_ref( ecl_file->kw_list , ecl_kw , ecl_kw_free__);
    
    first_kw = false;
  } while (ecl_kw != NULL);

  
  
  /* Returning NULL for an empty file. */
  if (vector_get_size(ecl_file->kw_list) == 0) {
    ecl_file_free( ecl_file );
    ecl_file = NULL;
  } else
    /* Building up the index for keyword/occurence based lookup. */
    ecl_file_make_index( ecl_file );
    
  
  return ecl_file;
}






/*
  This is the generic - "load a file" function. It will take ANY
  ECLIPSE file with ecl_kw format and internalize all the keywords as
  ecl_kw instances in one long vector.
*/

ecl_file_type * ecl_file_fread_alloc(const char * filename , bool endian_flip) {
  bool          fmt_file   = ecl_util_fmt_file( filename );
  fortio_type * fortio     = fortio_fopen(filename , "r" , endian_flip , fmt_file);
  
  ecl_file_type * ecl_file = ecl_file_fread_alloc_fortio(fortio , NULL);
    
  fortio_fclose( fortio );
  return ecl_file;
}



/* 
   This function will allocate a ecl_file_type instance going all the
   way to the NEXT 'SEQHDR' keyword. Observe that it is assumed that
   the fortio instance is already positioned at a SEQHDR keyword.
*/

ecl_file_type * ecl_file_fread_alloc_summary_section(fortio_type * fortio) {
  return ecl_file_fread_alloc_fortio(fortio , "SEQHDR");
}


/*
  The SEQNUM number found in unified restart files corresponds to the 
  REPORT_STEP.
*/
ecl_file_type * ecl_file_fread_alloc_restart_section(fortio_type * fortio) {
  return ecl_file_fread_alloc_fortio(fortio , "SEQNUM");
}


ecl_file_type * ecl_file_fread_alloc_RFT_section(fortio_type * fortio) {
  return ecl_file_fread_alloc_fortio(fortio , "TIME");
}

/*****************************************************************/
/* fwrite functions */

void ecl_file_fwrite_fortio(const ecl_file_type * ecl_file , fortio_type * fortio, int offset) {
  int index;
  for (index = offset; index < vector_get_size( ecl_file->kw_list ); index++)
    ecl_kw_fwrite( vector_iget( ecl_file->kw_list , index ) , fortio);
}


/* 
   Observe : if the filename is a standard filename which can be used
   to infer formatted/unformatted automagically the fmt_file variable
   is NOT consulted.
*/
  
void ecl_file_fwrite(const ecl_file_type * ecl_file , const char * filename, bool fmt_file, bool endian_flip) {
  bool __fmt_file;
  ecl_file_enum file_type;
  
  ecl_util_get_file_type( filename , &file_type , &__fmt_file , NULL);
  if (file_type == ecl_other_file)
    __fmt_file = fmt_file;
  
  {
    fortio_type * fortio = fortio_fopen( filename , "w", endian_flip , __fmt_file);
    ecl_file_fwrite_fortio( ecl_file , fortio , 0);
    fortio_fclose( fortio );
  }
}




void ecl_file_free(ecl_file_type * ecl_file) {
  vector_free( ecl_file->kw_list );
  hash_free( ecl_file->kw_index );
  util_safe_free( ecl_file->src_file );
  stringlist_free( ecl_file->distinct_kw );
  free(ecl_file);
}


void ecl_file_free__(void * arg) {
  ecl_file_free( ecl_file_safe_cast( arg ) );
}



/*****************************************************************/
/**
  Here comes several functions for querying the ecl_file instance, and
  getting pointers to the ecl_kw content of the ecl_file. For getting
  ecl_kw instances there are two principally different access methods:

    * ecl_file_iget_named_kw(): This function will take a keyword
      (char *) and an integer as input. The integer corresponds to the
      ith occurence of the keyword in the file.

    * ecl_file_iget_kw(): This function just takes an integer index as
      input, and returns the corresponding ecl_kw instance - without
      considering which keyword it is.

  -------

  In addition the functions ecl_file_get_num_distinct_kw() and
  ecl_file_iget_distinct_kw() will return the number of distinct
  keywords, and distinct keyword keyword nr i (as a const char *).


  Possible usage pattern:

  ....
  for (ikw = 0; ikw < ecl_file_get_num_distinct_kw(ecl_file); ikw++) {
     const char * kw = ecl_file_iget_distinct_kw(ecl_file , ikw);
     
     printf("The file contains: %d occurences of \'%s\' \n",ecl_file_get_num_named_kw( ecl_file , kw) , kw);
  }
  ....

  For the summary file showed in the top this code will produce:

    The file contains 1 occurences of 'SEQHDR'
    The file contains 3 occurences of 'MINISTEP'
    The file contains 3 occurences of 'PARAMS'
  
*/




/* 
   This function will return the ith occurence of 'kw' in
   ecl_file. Will abort hard if the request can not be satisifed - use
   query functions if you can not take that.
*/
   

ecl_kw_type * ecl_file_iget_named_kw( const ecl_file_type * ecl_file , const char * kw, int ith) {
  ecl_file_assert_type(ecl_file);
  {
    const int_vector_type * index_vector = hash_get(ecl_file->kw_index , kw);
    int global_index = int_vector_iget( index_vector , ith);
    return vector_iget( ecl_file->kw_list , global_index );
  }
}


  
/*
  Will return the number of times a particular keyword occurs in a
  ecl_file instance. Will return 0 if the keyword can not be found.
*/

int ecl_file_get_num_named_kw(const ecl_file_type * ecl_file , const char * kw) {
  if (hash_has_key(ecl_file->kw_index , kw)) {
    const int_vector_type * index_vector = hash_get(ecl_file->kw_index , kw);
    return int_vector_size( index_vector );
  } else
    return 0;
}



/**
   This will just return ecl_kw nr i - without looking at the names.
*/
ecl_kw_type * ecl_file_iget_kw( const ecl_file_type * ecl_file , int index) {
  return vector_iget( ecl_file->kw_list , index);
}



/**
   This function does the following:

    1. Takes an input index which goes in to the global kw_list vector.
    2. Looks up the corresponding keyword.
    3. Return the number of this particular keyword instance, among
       the other instance with the same header.

   With the example above we get:

     ecl_file_iget_occurence(ecl_file , 2) -> 0; Global index 2 will
        look up the first occurence of PARAMS.
  
     ecl_file_iget_occurence(ecl_file , 5) -> 2; Global index 5 will
        look up th third occurence of MINISTEP.

   The enkf layer uses this funny functionality.
*/


int ecl_file_iget_occurence( const ecl_file_type * ecl_file , int index) {
  const ecl_kw_type * ecl_kw = ecl_file_iget_kw( ecl_file , index );
  char * header = ecl_kw_alloc_strip_header( ecl_kw );
  const int_vector_type * index_vector = hash_get(ecl_file->kw_index , header );
  const int * index_data = int_vector_get_const_ptr( index_vector );
  
  int occurence = -1;
  {
    /* Manual reverse lookup. */
    for (int i=0; i < int_vector_size( index_vector ); i++)
      if (index_data[i] == index)
	occurence = i;
  }
  if (occurence < 0)
    util_abort("%s: internal error ... \n" , __func__);

  free(header);
  return occurence;
}


/** 
    Returns the total number of ecl_kw instances in the ecl_file
    instance.
*/
int ecl_file_get_num_kw( const ecl_file_type * ecl_file ){
  return vector_get_size( ecl_file->kw_list );
}


/**
   Returns true if the ecl_file instance has at-least one occurence of
   ecl_kw 'kw'.
*/
bool ecl_file_has_kw( const ecl_file_type * ecl_file , const char * kw) {
  return hash_has_key( ecl_file->kw_index , kw );
}


int ecl_file_get_num_distinct_kw(const ecl_file_type * ecl_file) {
  return stringlist_get_size( ecl_file->distinct_kw );
}


const char * ecl_file_iget_distinct_kw(const ecl_file_type * ecl_file, int index) {
  return stringlist_iget( ecl_file->distinct_kw , index);
}


