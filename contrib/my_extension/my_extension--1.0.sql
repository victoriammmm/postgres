CREATE FUNCTION print_hello_sql()
RETURNS text
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN 'hello from sql';
END;
$$;

CREATE FUNCTION print_hello_c()
RETURNS text
LANGUAGE C
AS '$libdir/my_extension', 'print_hello_c';

CREATE FUNCTION set_value(integer)
RETURNS void
LANGUAGE C
AS '$libdir/my_extension', 'set_value';