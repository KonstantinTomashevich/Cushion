#line 1 "source/combined_features.c"

#line 35 "source/combined_features.c"
struct database_context_1_t
{
    
#line 49 "source/combined_features.c"
database_query_t query_value_type_a ;
#line 55 "source/combined_features.c"
database_query_t query_value_type_b ;
#line 37 "source/combined_features.c"

};

struct database_context_2_t
{
    
#line 97 "source/combined_features.c"
database_query_t query_value_type_x ;
#line 107 "source/combined_features.c"
database_query_t query_value_type_y ;
#line 112 "source/combined_features.c"
database_query_t query_prefix__abc__suffix_t ;
#line 42 "source/combined_features.c"

};



void database_function_1_a (struct database_context_1_t *database_context_ptr)
{
    { database_read_cursor_t cursor_q_1 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 1 ) ; while ( 1 ) { database_read_access_t access_q_1 = database_read_cursor_resolve ( cursor_q_1 ) ; database_read_cursor_advance ( cursor_q_1 ) ; const struct value_type_a * q_1 = database_read_access_resolve ( access_q_1 ) ; if ( q_1 ) {

        
        
#line 49 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 52 "source/combined_features.c"
break;
    
#line 49 "source/combined_features.c"
} else { break ; } } 
#line 49 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 49 "source/combined_features.c"
}
#line 55 "source/combined_features.c"
    { database_read_cursor_t cursor_q_2 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_b , 2 ) ; while ( 1 ) { database_read_access_t access_q_2 = database_read_cursor_resolve ( cursor_q_2 ) ; database_read_cursor_advance ( cursor_q_2 ) ; const struct value_type_b * q_2 = database_read_access_resolve ( access_q_2 ) ; if ( q_2 ) {

        
        
#line 55 "source/combined_features.c"
database_read_access_close ( access_q_2 ) ;
#line 58 "source/combined_features.c"
continue;
    
#line 55 "source/combined_features.c"
} else { break ; } } 
#line 55 "source/combined_features.c"
database_read_cursor_close ( cursor_q_2 ) ;
#line 55 "source/combined_features.c"
}
#line 61 "source/combined_features.c"
    { database_read_cursor_t cursor_q_3 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 3 ) ; while ( 1 ) { database_read_access_t access_q_3 = database_read_cursor_resolve ( cursor_q_3 ) ; database_read_cursor_advance ( cursor_q_3 ) ; const struct value_type_a * q_3 = database_read_access_resolve ( access_q_3 ) ; if ( q_3 ) {

        
        
#line 61 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 61 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 64 "source/combined_features.c"
return;
    
#line 61 "source/combined_features.c"
} else { break ; } } 
#line 61 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 61 "source/combined_features.c"
}
#line 67 "source/combined_features.c"
    return;
}

void database_function_1_b (struct database_context_1_t *database_context_ptr)
{
    { database_read_cursor_t cursor_q_1 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_b , 1 ) ; while ( 1 ) { database_read_access_t access_q_1 = database_read_cursor_resolve ( cursor_q_1 ) ; database_read_cursor_advance ( cursor_q_1 ) ; const struct value_type_b * q_1 = database_read_access_resolve ( access_q_1 ) ; if ( q_1 ) {

        { database_read_cursor_t cursor_q_2 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 4 ) ; while ( 1 ) { database_read_access_t access_q_2 = database_read_cursor_resolve ( cursor_q_2 ) ; database_read_cursor_advance ( cursor_q_2 ) ; const struct value_type_a * q_2 = database_read_access_resolve ( access_q_2 ) ; if ( q_2 ) {

            
            
#line 74 "source/combined_features.c"
database_read_access_close ( access_q_2 ) ;
#line 77 "source/combined_features.c"
break;
        
#line 74 "source/combined_features.c"
} else { break ; } } 
#line 74 "source/combined_features.c"
database_read_cursor_close ( cursor_q_2 ) ;
#line 74 "source/combined_features.c"
}
#line 80 "source/combined_features.c"
        { database_read_cursor_t cursor_q_3 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 4 ) ; while ( 1 ) { database_read_access_t access_q_3 = database_read_cursor_resolve ( cursor_q_3 ) ; database_read_cursor_advance ( cursor_q_3 ) ; const struct value_type_a * q_3 = database_read_access_resolve ( access_q_3 ) ; if ( q_3 ) {

            { database_read_cursor_t cursor_q_4 = database_query_execute_read ( ( database_context_ptr ) -> query_value_type_a , 5 ) ; while ( 1 ) { database_read_access_t access_q_4 = database_read_cursor_resolve ( cursor_q_4 ) ; database_read_cursor_advance ( cursor_q_4 ) ; const struct value_type_a * q_4 = database_read_access_resolve ( access_q_4 ) ; if ( q_4 ) {

                
                typeof ( 5) cushion_cached_return_value_0 =  5;
#line 82 "source/combined_features.c"
database_read_access_close ( access_q_4 ) ;
#line 82 "source/combined_features.c"
database_read_cursor_close ( cursor_q_4 ) ;
#line 80 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 80 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 72 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 72 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 85 "source/combined_features.c"
return cushion_cached_return_value_0;
            
#line 82 "source/combined_features.c"
} else { break ; } } 
#line 82 "source/combined_features.c"
database_read_cursor_close ( cursor_q_4 ) ;
#line 82 "source/combined_features.c"
}
#line 87 "source/combined_features.c"
        
#line 80 "source/combined_features.c"

#line 80 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 80 "source/combined_features.c"
} else { break ; } } 
#line 80 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 80 "source/combined_features.c"
}
#line 88 "source/combined_features.c"
    
#line 72 "source/combined_features.c"

#line 72 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 72 "source/combined_features.c"
} else { break ; } } 
#line 72 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 72 "source/combined_features.c"
}
#line 89 "source/combined_features.c"
}
#line 95 "source/combined_features.c"
void database_function_2 (struct database_context_2_t *database_context_ref)
{
    { database_read_cursor_t cursor_q_1 = database_query_execute_read ( ( database_context_ref ) -> query_value_type_x , 10 ) ; while ( 1 ) { database_read_access_t access_q_1 = database_read_cursor_resolve ( cursor_q_1 ) ; database_read_cursor_advance ( cursor_q_1 ) ; const struct value_type_x * q_1 = database_read_access_resolve ( access_q_1 ) ; if ( q_1 ) {

        
        int a = 19;
        int b = 21;
        int x = a + b;
        int y = x + a + b;
        a = b = x = y;
    
#line 97 "source/combined_features.c"

#line 97 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 97 "source/combined_features.c"
} else { break ; } } 
#line 97 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 97 "source/combined_features.c"
}
#line 107 "source/combined_features.c"
    { database_read_cursor_t cursor_q_2 = database_query_execute_read ( ( database_context_ref ) -> query_value_type_y , 20 ) ; while ( 1 ) { database_read_access_t access_q_2 = database_read_cursor_resolve ( cursor_q_2 ) ; database_read_cursor_advance ( cursor_q_2 ) ; const struct value_type_y * q_2 = database_read_access_resolve ( access_q_2 ) ; if ( q_2 ) {

        
    
#line 107 "source/combined_features.c"

#line 107 "source/combined_features.c"
database_read_access_close ( access_q_2 ) ;
#line 107 "source/combined_features.c"
} else { break ; } } 
#line 107 "source/combined_features.c"
database_read_cursor_close ( cursor_q_2 ) ;
#line 107 "source/combined_features.c"
}
#line 112 "source/combined_features.c"
    { database_read_cursor_t cursor_q_3 = database_query_execute_read ( ( database_context_ref ) -> query_prefix__abc__suffix_t , 20 ) ; while ( 1 ) { database_read_access_t access_q_3 = database_read_cursor_resolve ( cursor_q_3 ) ; database_read_cursor_advance ( cursor_q_3 ) ; const struct prefix__abc__suffix_t * q_3 = database_read_access_resolve ( access_q_3 ) ; if ( q_3 ) {

        
    
#line 112 "source/combined_features.c"

#line 112 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 112 "source/combined_features.c"
} else { break ; } } 
#line 112 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 112 "source/combined_features.c"
}



}
