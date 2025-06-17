#line 1 "source/combined_features.c"

#line 31 "source/combined_features.c"
struct database_context_1_t
{
    
#line 45 "source/combined_features.c"
database_query_t query_value_type_a ;
#line 51 "source/combined_features.c"
database_query_t query_value_type_b ;
#line 33 "source/combined_features.c"

};

struct database_context_2_t
{
    
#line 91 "source/combined_features.c"
database_query_t query_value_type_x ;
#line 101 "source/combined_features.c"
database_query_t query_value_type_y ;
#line 38 "source/combined_features.c"

};



void database_function_1_a (struct database_context_1_t *database_context)
{
    { database_read_cursor_t cursor_q_1 = database_query_execute_read ( database_context -> query_value_type_a , 1 ) ; while ( 1 ) { database_read_access_t access_q_1 = database_read_cursor_resolve ( cursor_q_1 ) ; database_read_cursor_advance ( cursor_q_1 ) ; const struct value_type_a * q_1 = database_read_access_resolve ( access_q_1 ) ; if ( q_1 ) {

        
        
#line 45 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 48 "source/combined_features.c"
break;
    
#line 45 "source/combined_features.c"
} else { break ; } } 
#line 45 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 45 "source/combined_features.c"
}
#line 51 "source/combined_features.c"
    { database_read_cursor_t cursor_q_2 = database_query_execute_read ( database_context -> query_value_type_b , 2 ) ; while ( 1 ) { database_read_access_t access_q_2 = database_read_cursor_resolve ( cursor_q_2 ) ; database_read_cursor_advance ( cursor_q_2 ) ; const struct value_type_b * q_2 = database_read_access_resolve ( access_q_2 ) ; if ( q_2 ) {

        
        
#line 51 "source/combined_features.c"
database_read_access_close ( access_q_2 ) ;
#line 54 "source/combined_features.c"
continue;
    
#line 51 "source/combined_features.c"
} else { break ; } } 
#line 51 "source/combined_features.c"
database_read_cursor_close ( cursor_q_2 ) ;
#line 51 "source/combined_features.c"
}
#line 57 "source/combined_features.c"
    { database_read_cursor_t cursor_q_3 = database_query_execute_read ( database_context -> query_value_type_a , 3 ) ; while ( 1 ) { database_read_access_t access_q_3 = database_read_cursor_resolve ( cursor_q_3 ) ; database_read_cursor_advance ( cursor_q_3 ) ; const struct value_type_a * q_3 = database_read_access_resolve ( access_q_3 ) ; if ( q_3 ) {

        
        
#line 57 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 57 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 60 "source/combined_features.c"
return;
    
#line 57 "source/combined_features.c"
} else { break ; } } 
#line 57 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 57 "source/combined_features.c"
}
#line 63 "source/combined_features.c"
    return;
}

void database_function_1_b (struct database_context_1_t *database_context)
{
    { database_read_cursor_t cursor_q_1 = database_query_execute_read ( database_context -> query_value_type_b , 1 ) ; while ( 1 ) { database_read_access_t access_q_1 = database_read_cursor_resolve ( cursor_q_1 ) ; database_read_cursor_advance ( cursor_q_1 ) ; const struct value_type_b * q_1 = database_read_access_resolve ( access_q_1 ) ; if ( q_1 ) {

        { database_read_cursor_t cursor_q_2 = database_query_execute_read ( database_context -> query_value_type_a , 4 ) ; while ( 1 ) { database_read_access_t access_q_2 = database_read_cursor_resolve ( cursor_q_2 ) ; database_read_cursor_advance ( cursor_q_2 ) ; const struct value_type_a * q_2 = database_read_access_resolve ( access_q_2 ) ; if ( q_2 ) {

            
            
#line 70 "source/combined_features.c"
database_read_access_close ( access_q_2 ) ;
#line 73 "source/combined_features.c"
break;
        
#line 70 "source/combined_features.c"
} else { break ; } } 
#line 70 "source/combined_features.c"
database_read_cursor_close ( cursor_q_2 ) ;
#line 70 "source/combined_features.c"
}
#line 76 "source/combined_features.c"
        { database_read_cursor_t cursor_q_3 = database_query_execute_read ( database_context -> query_value_type_a , 4 ) ; while ( 1 ) { database_read_access_t access_q_3 = database_read_cursor_resolve ( cursor_q_3 ) ; database_read_cursor_advance ( cursor_q_3 ) ; const struct value_type_a * q_3 = database_read_access_resolve ( access_q_3 ) ; if ( q_3 ) {

            { database_read_cursor_t cursor_q_4 = database_query_execute_read ( database_context -> query_value_type_a , 5 ) ; while ( 1 ) { database_read_access_t access_q_4 = database_read_cursor_resolve ( cursor_q_4 ) ; database_read_cursor_advance ( cursor_q_4 ) ; const struct value_type_a * q_4 = database_read_access_resolve ( access_q_4 ) ; if ( q_4 ) {

                
                typeof ( 5) cushion_cached_return_value_0 =  5;
#line 78 "source/combined_features.c"
database_read_access_close ( access_q_4 ) ;
#line 78 "source/combined_features.c"
database_read_cursor_close ( cursor_q_4 ) ;
#line 76 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 76 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 68 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 68 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 81 "source/combined_features.c"
return cushion_cached_return_value_0;
            
#line 78 "source/combined_features.c"
} else { break ; } } 
#line 78 "source/combined_features.c"
database_read_cursor_close ( cursor_q_4 ) ;
#line 78 "source/combined_features.c"
}
#line 83 "source/combined_features.c"
        
#line 76 "source/combined_features.c"

#line 76 "source/combined_features.c"
database_read_access_close ( access_q_3 ) ;
#line 76 "source/combined_features.c"
} else { break ; } } 
#line 76 "source/combined_features.c"
database_read_cursor_close ( cursor_q_3 ) ;
#line 76 "source/combined_features.c"
}
#line 84 "source/combined_features.c"
    
#line 68 "source/combined_features.c"

#line 68 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 68 "source/combined_features.c"
} else { break ; } } 
#line 68 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 68 "source/combined_features.c"
}
#line 85 "source/combined_features.c"
}



void database_function_2 (struct database_context_2_t *database_context)
{
    { database_read_cursor_t cursor_q_1 = database_query_execute_read ( database_context -> query_value_type_x , 10 ) ; while ( 1 ) { database_read_access_t access_q_1 = database_read_cursor_resolve ( cursor_q_1 ) ; database_read_cursor_advance ( cursor_q_1 ) ; const struct value_type_x * q_1 = database_read_access_resolve ( access_q_1 ) ; if ( q_1 ) {

        
        int a = 19;
        int b = 21;
        int x = a + b;
        int y = x + a + b;
        a = b = x = y;
    
#line 91 "source/combined_features.c"

#line 91 "source/combined_features.c"
database_read_access_close ( access_q_1 ) ;
#line 91 "source/combined_features.c"
} else { break ; } } 
#line 91 "source/combined_features.c"
database_read_cursor_close ( cursor_q_1 ) ;
#line 91 "source/combined_features.c"
}
#line 101 "source/combined_features.c"
    { database_read_cursor_t cursor_q_2 = database_query_execute_read ( database_context -> query_value_type_y , 20 ) ; while ( 1 ) { database_read_access_t access_q_2 = database_read_cursor_resolve ( cursor_q_2 ) ; database_read_cursor_advance ( cursor_q_2 ) ; const struct value_type_y * q_2 = database_read_access_resolve ( access_q_2 ) ; if ( q_2 ) {

        
    
#line 101 "source/combined_features.c"

#line 101 "source/combined_features.c"
database_read_access_close ( access_q_2 ) ;
#line 101 "source/combined_features.c"
} else { break ; } } 
#line 101 "source/combined_features.c"
database_read_cursor_close ( cursor_q_2 ) ;
#line 101 "source/combined_features.c"
}



}
