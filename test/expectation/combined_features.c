#line 1 "source/combined_features.c"

#line 39 "source/combined_features.c"
struct database_context_1_t
{
    
#line 54 "source/combined_features.c"
database_query_t query_value_type_a ;
#line 60 "source/combined_features.c"
database_query_t query_value_type_b ;
#line 41 "source/combined_features.c"

};

struct database_context_2_t
{
    
#line 106 "source/combined_features.c"
database_query_t query_value_type_x ;
#line 116 "source/combined_features.c"
database_query_t query_value_type_y ;
#line 121 "source/combined_features.c"
database_query_t query_prefix__abc__suffix_t ;
#line 46 "source/combined_features.c"

};



void database_function_1_a (struct database_context_1_t *database_context_ptr)
{
    profiler_scope_handle_t profiler_scope_1 = profile_scope_begin ( "test_1" ) ; ;
    { database_read_cursor_t cursor_q_1 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 1 ) ; while ( 1 ) { database_read_access_t access_q_1 = database_read_cursor_resolve ( cursor_q_1 ) ; database_read_cursor_advance ( cursor_q_1 ) ; const struct value_type_a * q_1 = database_read_access_resolve ( access_q_1 ) ; if ( q_1 ) {

        
        
#line 54 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 57 "source/combined_features.c"
break;
    
#line 54 "source/combined_features.c"
} else { break ; } } 
#line 54 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 54 "source/combined_features.c"
}
#line 60 "source/combined_features.c"
    { database_read_cursor_t cursor_q_2 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_b , 2 ) ; while ( 1 ) { database_read_access_t access_q_2 = database_read_cursor_resolve ( cursor_q_2 ) ; database_read_cursor_advance ( cursor_q_2 ) ; const struct value_type_b * q_2 = database_read_access_resolve ( access_q_2 ) ; if ( q_2 ) {

        
        
#line 60 "source/combined_features.c"
database_read_access_close ( access_q_2 ) ;
#line 63 "source/combined_features.c"
continue;
    
#line 60 "source/combined_features.c"
} else { break ; } } 
#line 60 "source/combined_features.c"
database_read_cursor_close ( cursor_q_2 ) ;
#line 60 "source/combined_features.c"
}
#line 66 "source/combined_features.c"
    { database_read_cursor_t cursor_q_3 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 3 ) ; while ( 1 ) { database_read_access_t access_q_3 = database_read_cursor_resolve ( cursor_q_3 ) ; database_read_cursor_advance ( cursor_q_3 ) ; const struct value_type_a * q_3 = database_read_access_resolve ( access_q_3 ) ; if ( q_3 ) {

        profiler_scope_handle_t profiler_scope_8 = profile_scope_begin ( "test_2" ) ; ;
        
        
#line 68 "source/combined_features.c"
profiler_scope_end ( profiler_scope_8 ) ;
#line 66 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 66 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 53 "source/combined_features.c"
profiler_scope_end ( profiler_scope_1 ) ;
#line 70 "source/combined_features.c"
return;
    
#line 66 "source/combined_features.c"
} else { break ; } } 
#line 66 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 66 "source/combined_features.c"
}
#line 73 "source/combined_features.c"
    
#line 53 "source/combined_features.c"
profiler_scope_end ( profiler_scope_1 ) ;
#line 73 "source/combined_features.c"
return;
}

void database_function_1_b (struct database_context_1_t *database_context_ptr)
{
    profiler_scope_handle_t profiler_scope_9 = profile_scope_begin ( "a" ) ; ;
    { database_read_cursor_t cursor_q_1 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_b , 1 ) ; while ( 1 ) { database_read_access_t access_q_1 = database_read_cursor_resolve ( cursor_q_1 ) ; database_read_cursor_advance ( cursor_q_1 ) ; const struct value_type_b * q_1 = database_read_access_resolve ( access_q_1 ) ; if ( q_1 ) {

        profiler_scope_handle_t profiler_scope_12 = profile_scope_begin ( "b" ) ; ;
        { database_read_cursor_t cursor_q_2 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 4 ) ; while ( 1 ) { database_read_access_t access_q_2 = database_read_cursor_resolve ( cursor_q_2 ) ; database_read_cursor_advance ( cursor_q_2 ) ; const struct value_type_a * q_2 = database_read_access_resolve ( access_q_2 ) ; if ( q_2 ) {

            
            
#line 82 "source/combined_features.c"
database_read_access_close ( access_q_2 ) ;
#line 85 "source/combined_features.c"
break;
        
#line 82 "source/combined_features.c"
} else { break ; } } 
#line 82 "source/combined_features.c"
database_read_cursor_close ( cursor_q_2 ) ;
#line 82 "source/combined_features.c"
}
#line 88 "source/combined_features.c"
        { database_read_cursor_t cursor_q_3 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 4 ) ; while ( 1 ) { database_read_access_t access_q_3 = database_read_cursor_resolve ( cursor_q_3 ) ; database_read_cursor_advance ( cursor_q_3 ) ; const struct value_type_a * q_3 = database_read_access_resolve ( access_q_3 ) ; if ( q_3 ) {

            profiler_scope_handle_t profiler_scope_17 = profile_scope_begin ( "c" ) ; ;
            { database_read_cursor_t cursor_q_4 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 5 ) ; while ( 1 ) { database_read_access_t access_q_4 = database_read_cursor_resolve ( cursor_q_4 ) ; database_read_cursor_advance ( cursor_q_4 ) ; const struct value_type_a * q_4 = database_read_access_resolve ( access_q_4 ) ; if ( q_4 ) {

                
                typeof ( 5) cushion_cached_return_value_0 =  5;
#line 91 "source/combined_features.c"
database_read_access_close ( access_q_4 ) ;
#line 91 "source/combined_features.c"
database_read_cursor_close ( cursor_q_4 ) ;
#line 90 "source/combined_features.c"
profiler_scope_end ( profiler_scope_17 ) ;
#line 88 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 88 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 81 "source/combined_features.c"
profiler_scope_end ( profiler_scope_12 ) ;
#line 79 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 79 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 78 "source/combined_features.c"
profiler_scope_end ( profiler_scope_9 ) ;
#line 94 "source/combined_features.c"
return cushion_cached_return_value_0;
            
#line 91 "source/combined_features.c"
} else { break ; } } 
#line 91 "source/combined_features.c"
database_read_cursor_close ( cursor_q_4 ) ;
#line 91 "source/combined_features.c"
}
#line 96 "source/combined_features.c"
        
#line 88 "source/combined_features.c"

#line 90 "source/combined_features.c"
profiler_scope_end ( profiler_scope_17 ) ;
#line 88 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 88 "source/combined_features.c"
} else { break ; } } 
#line 88 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 88 "source/combined_features.c"
}
#line 97 "source/combined_features.c"
    
#line 79 "source/combined_features.c"

#line 81 "source/combined_features.c"
profiler_scope_end ( profiler_scope_12 ) ;
#line 79 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 79 "source/combined_features.c"
} else { break ; } } 
#line 79 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 79 "source/combined_features.c"
}
#line 98 "source/combined_features.c"

#line 78 "source/combined_features.c"
profiler_scope_end ( profiler_scope_9 ) ;
#line 98 "source/combined_features.c"
}
#line 104 "source/combined_features.c"
void database_function_2 (struct database_context_2_t *database_context_ref)
{
    { database_read_cursor_t cursor_q_1 = database_query_execute_read ( ( database_context_ref ) -> query_value_type_x , 10 ) ; while ( 1 ) { database_read_access_t access_q_1 = database_read_cursor_resolve ( cursor_q_1 ) ; database_read_cursor_advance ( cursor_q_1 ) ; const struct value_type_x * q_1 = database_read_access_resolve ( access_q_1 ) ; if ( q_1 ) {

        
        int a = 19;
        int b = 21;
        int x = a + b;
        int y = x + a + b;
        a = b = x = y;
    
#line 106 "source/combined_features.c"

#line 106 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 106 "source/combined_features.c"
} else { break ; } } 
#line 106 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 106 "source/combined_features.c"
}
#line 116 "source/combined_features.c"
    { database_read_cursor_t cursor_q_2 = database_query_execute_read ( ( database_context_ref ) -> query_value_type_y , 20 ) ; while ( 1 ) { database_read_access_t access_q_2 = database_read_cursor_resolve ( cursor_q_2 ) ; database_read_cursor_advance ( cursor_q_2 ) ; const struct value_type_y * q_2 = database_read_access_resolve ( access_q_2 ) ; if ( q_2 ) {

        
    
#line 116 "source/combined_features.c"

#line 116 "source/combined_features.c"
database_read_access_close ( access_q_2 ) ;
#line 116 "source/combined_features.c"
} else { break ; } } 
#line 116 "source/combined_features.c"
database_read_cursor_close ( cursor_q_2 ) ;
#line 116 "source/combined_features.c"
}
#line 121 "source/combined_features.c"
    { database_read_cursor_t cursor_q_3 = database_query_execute_read ( ( database_context_ref ) -> query_prefix__abc__suffix_t , 20 ) ; while ( 1 ) { database_read_access_t access_q_3 = database_read_cursor_resolve ( cursor_q_3 ) ; database_read_cursor_advance ( cursor_q_3 ) ; const struct prefix__abc__suffix_t * q_3 = database_read_access_resolve ( access_q_3 ) ; if ( q_3 ) {

        
    
#line 121 "source/combined_features.c"

#line 121 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 121 "source/combined_features.c"
} else { break ; } } 
#line 121 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 121 "source/combined_features.c"
}



}
